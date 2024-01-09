import coalpy.gpu as g
from . import utilities as utils

g_group_size = 128
g_batch_size = 1024
g_bits_per_radix = 8
g_bytes_per_radix = int(g_bits_per_radix/8)
g_radix_counts = int(1 << g_bits_per_radix)
g_radix_iterations = int(32/g_bits_per_radix)

g_write_indirect_args_shader = None
g_count_scatter_shader = None
g_prefix_count_table_shader = None
g_prefix_global_table_shader = None
g_scatter_output_shader = None

def init():
    global g_write_indirect_args_shader
    global g_count_scatter_shader
    global g_prefix_count_table_shader
    global g_prefix_global_table_shader
    global g_scatter_output_shader

    g_write_indirect_args_shader = g.Shader(file = "shaders/radix_sort.hlsl", main_function = "csWriteIndirectArguments")
    g_count_scatter_shader = g.Shader(file = "shaders/radix_sort.hlsl", main_function = "csCountScatterBuckets")
    g_prefix_count_table_shader = g.Shader(file = "shaders/radix_sort.hlsl", main_function = "csPrefixCountTable", defines = ["GROUP_SIZE=256"])
    g_prefix_global_table_shader = g.Shader(file = "shaders/radix_sort.hlsl", main_function = "csPrefixGlobalTable", defines = ["GROUP_SIZE=RADIX_COUNTS"])
    g_scatter_output_shader = g.Shader(file = "shaders/radix_sort.hlsl", main_function = "csScatterOutput", defines=["GROUP_SIZE="+str(g_batch_size)])

# Must match flags in radix_sort.hlsl 
FLAGS_IS_FIRST_PASS = 1 << 0
FLAGS_OUTPUT_ORDERING = 1 << 1

def allocate_args(input_counts, output_ordering = False, is_indirect = False):
    aligned_batch_count = utils.divup(input_counts, g_batch_size)
    count_table_count = aligned_batch_count * g_radix_counts
    return (
        g.Buffer(name="localOffsetsBuffer", element_count = input_counts, format = g.Format.R32_UINT),
        g.Buffer(name="pingBuffer", element_count = input_counts, format = g.Format.R32_UINT),
        g.Buffer(name="pongBuffer", element_count = input_counts, format = g.Format.R32_UINT),
        g.Buffer(name="countTableBatchPrefixBuffer", element_count = count_table_count, format = g.Format.R32_UINT),
        g.Buffer(name="radixTotalCounts", element_count = g_radix_counts, format = g.Format.R32_UINT),
        g.Buffer(name="countTableBuffer", element_count = count_table_count, format = g.Format.R32_UINT),
        g.Buffer(name="sortConstants", element_count = 8, format = g.Format.R32_UINT, usage = g.BufferUsage.Constant),
        g.Buffer(name="IndirectArgs", element_count = 4, format = g.Format.R32_UINT, usage = g.BufferUsage.IndirectArgs) if is_indirect else None,
        input_counts,
        output_ordering)


def run (cmd_list, input_buffer, sort_args, indirect_count_buffer = None):
    (
        local_offsets,
        ping_buffer,
        pong_buffer,
        count_table_prefix,
        radix_total_counts,
        count_table,
        constant_buffer,
        indirect_args,
        input_counts,
        output_ordering
    ) = sort_args

    if indirect_count_buffer == None and indirect_args != None:
        raise Exception("Indirect buffer has to be provided when the sorting uses indirect arguments.")

    batch_counts = utils.divup(input_counts, g_batch_size)

    radix_mask = int((1 << g_bits_per_radix) - 1)

    tmp_input_buffer = ping_buffer
    tmp_output_buffer = pong_buffer

    constant_data  = [
        int(input_counts), # g_inputCount
        int(batch_counts), # g_batchCount
        int(radix_mask), # g_radixMask
        int(0), # g_unused0
        int(0), # g_radixShift, set to 0
        int(0), # g_flags, set to 0
        int(0),# g_unused1
        int(0) ]# g_unused2

    cmd_list.upload_resource( source = constant_data, destination=constant_buffer )

    if indirect_args != None:
        cmd_list.dispatch(
            x = 1, y = 1, z = 1,
            shader = g_write_indirect_args_shader,
            inputs = indirect_count_buffer,
            outputs = [ constant_buffer, indirect_args ])

    for radix_i in range(0, g_radix_iterations):
        radix_shift = g_bits_per_radix * radix_i
        flags = FLAGS_IS_FIRST_PASS if radix_i == 0 else 0
        flags = flags | (FLAGS_OUTPUT_ORDERING if output_ordering else 0)

        (tmp_input_buffer, tmp_output_buffer) = (tmp_output_buffer, tmp_input_buffer)

        unsorted_buffer = None
        input_ordering = None

        if (flags & FLAGS_OUTPUT_ORDERING) == 0:
            unsorted_buffer = input_buffer if radix_i == 0 else tmp_input_buffer
            input_ordering = unsorted_buffer # unused so we set it as the unsorted buffer
        else:
            unsorted_buffer = input_buffer
            input_ordering = tmp_input_buffer


        #patch constant data, only elements that change
        constant_data_patch  = [
            int(radix_shift), # g_radixShift
            int(flags), # g_flags
            int(0),
            int(0)
        ]
        cmd_list.upload_resource( source = constant_data_patch, destination=constant_buffer, destination_offset = 4 * 4 )

        cmd_list.begin_marker("count_scatter")

        if indirect_args == None:
            cmd_list.dispatch(
                x = batch_counts, y = 1, z = 1,
                shader = g_count_scatter_shader,
                inputs = [ unsorted_buffer, input_ordering ],
                outputs = [ local_offsets, count_table ],
                constants = constant_buffer
            )
        else:
            cmd_list.dispatch(
                indirect_args = indirect_args,
                shader = g_count_scatter_shader,
                inputs = [ unsorted_buffer, input_ordering ],
                outputs = [ local_offsets, count_table ],
                constants = constant_buffer
            )

        cmd_list.end_marker()

        cmd_list.begin_marker("prefix_batch_table")
        cmd_list.dispatch(
            x = int(g_radix_counts), y = 1, z = 1,
            shader = g_prefix_count_table_shader,
            inputs = count_table,
            outputs = [count_table_prefix, radix_total_counts],
            constants = constant_buffer
        )
        cmd_list.end_marker()

        cmd_list.begin_marker("prefix_global_table")
        cmd_list.dispatch(
            x = 1, y = 1, z = 1,
            shader = g_prefix_global_table_shader,
            inputs = radix_total_counts,
            outputs = count_table
        )
        cmd_list.end_marker()

        cmd_list.begin_marker("scatter_output")
        cmd_list.dispatch(
            x = batch_counts, y = 1, z = 1,
            shader = g_scatter_output_shader,
            inputs = [unsorted_buffer, input_ordering, local_offsets, count_table_prefix, count_table ],
            outputs = tmp_output_buffer,
            constants = constant_buffer
        )
        cmd_list.end_marker()

    return (tmp_output_buffer, radix_total_counts)
