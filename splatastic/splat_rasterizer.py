import coalpy.gpu as g
import math
from . import utilities
from . import camera
from . import radix_sort

g_coarse_tile_record_bytes = 512 * 1024 * 1024

CoarseTileSize = 32

class SplatRaster:

    def __init__(self):
        self.m_coarse_tile_args_buffer = None
        self.m_coarse_tile_records = None
        self.m_coarse_tile_record_splat_ids = None
        self.m_coarse_tile_records_counter = None
        self.m_coarse_tile_record_max = 0
        self.m_coarse_tile_list_ordering = None
        self.m_coarse_tile_list_ranges = None
        self.m_color_buffer = None
        self.m_constants = None
        self.m_max_width = 0
        self.m_max_height = 0
        self.m_radix_sort_args = None
        self.init_shaders()
        return

    @property
    def color_buffer(self):
        return self.m_color_buffer

    def init_shaders(self):
        self.m_coarse_dispatch_bin_shader = g.Shader(file = "shaders/splat_rasterizer_cs.hlsl", name="CoarseTileBin", main_function = "csCoarseTileBin")
        self.m_create_coarse_tile_args_shader = g.Shader(file = "shaders/splat_rasterizer_cs.hlsl", name="CreateCoarseTileDispatchArgs", main_function = "csCreateCoarseTileDispatchArgs")
        self.m_create_coarse_tile_list_ranges_shader = g.Shader(file = "shaders/splat_rasterizer_cs.hlsl", name="CreateCoarseTileList", main_function = "csCreateCoarseTileListRanges")
        self.m_raster_splat_shader = g.Shader(file = "shaders/splat_rasterizer_cs.hlsl", name="RasterSplats", main_function = "csRasterSplats")

    def update_constants(self, cmd_list, view_matrix, proj_matrix, width, height, coarse_tile_count_x, coarse_tile_count_y):
        constants_data = [
            int(width), int(height), float(1.0/width), float(1.0/height),
            int(coarse_tile_count_x), int(coarse_tile_count_y), float(1.0/coarse_tile_count_x), float(1.0/coarse_tile_count_y),
            int(self.m_coarse_tile_record_max), 0, 0, 0,
        ]

        constants_data.extend(view_matrix.transpose().flatten().tolist())
        constants_data.extend(proj_matrix.transpose().flatten().tolist())

        if self.m_constants == None:
            self.m_constants = g.Buffer(
                name = "SplatRasterConstants",
                stride = 4,
                element_count = len(constants_data),
                usage = g.BufferUsage.Constant)

        cmd_list.upload_resource(source = constants_data, destination =  self.m_constants)

    def update_view_resources(self, width, height, coarse_tile_count_x, coarse_tile_count_y):
        if self.m_coarse_tile_args_buffer is None:
            self.m_coarse_tile_args_buffer = g.Buffer(
                "CoarseTileArgsBuffer",
                format = g.Format.RGBA_32_UINT,
                usage = g.BufferUsage.IndirectArgs,
                element_count = 1)

        if self.m_coarse_tile_records is None:
            coarse_tile_record_stride = 4
            self.m_coarse_tile_record_max = utilities.divup(g_coarse_tile_record_bytes, coarse_tile_record_stride)
            self.m_coarse_tile_records = g.Buffer(
                "CoarseTileRecord",
                format = g.Format.R32_UINT,
                stride = coarse_tile_record_stride,
                element_count = self.m_coarse_tile_record_max)
            self.m_coarse_tile_record_splat_ids = g.Buffer(
                "CoarseTileSplatId",
                format = g.Format.R32_UINT,
                stride = coarse_tile_record_stride,
                element_count = self.m_coarse_tile_record_max)
            print ("Max records: " + str(self.m_coarse_tile_record_max))

        if self.m_coarse_tile_records_counter is None:
            self.m_coarse_tile_records_counter = g.Buffer(
                "CoarseTileRecordCounter",
                format = g.Format.R32_UINT,
                stride = 4,
                element_count = 1)

        if width <= self.m_max_width and height <= self.m_max_height:
            return

        self.m_radix_sort_args = radix_sort.allocate_args(self.m_coarse_tile_record_max, output_ordering = True, is_indirect = True)

        (self.m_max_width, self.m_max_height) = (width, height)

        self.m_color_buffer = g.Texture(
            "ColorBuffer",
            format = g.Format.RGBA_8_UNORM,
            width = self.m_max_width, height = self.m_max_height)

        self.m_coarse_tile_list_ranges = g.Buffer(
            "CoarseTileListRanges",
            format = g.Format.R32_UINT, stride = 4,
            element_count = 2 * coarse_tile_count_x * coarse_tile_count_y)

        return

    def clear_view_buffers(self, cmd_list, width, height, coarse_tile_count_x, coarse_tile_count_y):
        utilities.clear_uint_buffer(cmd_list, 0, self.m_coarse_tile_records_counter, 0, 1)
        utilities.clear_uint_buffer(cmd_list, 0, self.m_coarse_tile_list_ranges, 0, coarse_tile_count_x * coarse_tile_count_y * 2)

    def dispatch_coarse_tile_bin(self, cmd_list, scene_data, coarse_tile_count_x, coarse_tile_count_y):
    
        #keep in sync with csCoarseTileBin
        coarse_tile_bin_threads = 128

        cmd_list.begin_marker("coarse_tile_bin")

        cmd_list.dispatch(
            shader = self.m_coarse_dispatch_bin_shader,
            constants = self.m_constants,
            inputs = [ scene_data.metadata_buffer, scene_data.payload_buffer ],
            outputs = [ self.m_coarse_tile_records_counter, self.m_coarse_tile_records, self.m_coarse_tile_record_splat_ids ],
            x = utilities.divup(scene_data.vertex_count, coarse_tile_bin_threads), y = 1, z = 1)
        cmd_list.end_marker()

        cmd_list.begin_marker("radix_sort")
        (self.m_coarse_tile_list_ordering, _) = radix_sort.run(cmd_list, self.m_coarse_tile_records, self.m_radix_sort_args, indirect_count_buffer = self.m_coarse_tile_records_counter)
        cmd_list.end_marker()

        cmd_list.begin_marker("create_tile_args")
        cmd_list.dispatch(
            shader = self.m_create_coarse_tile_args_shader,
            inputs = self.m_coarse_tile_records_counter,
            outputs = self.m_coarse_tile_args_buffer,
            x = 1, y = 1, z = 1)
        cmd_list.end_marker()

        cmd_list.dispatch(
            shader = self.m_create_coarse_tile_list_ranges_shader,
            constants = self.m_constants,
            inputs = [
                self.m_coarse_tile_records_counter,
                self.m_coarse_tile_list_ordering,
                self.m_coarse_tile_records ],
            outputs = [ self.m_coarse_tile_list_ranges ],
            indirect_args = self.m_coarse_tile_args_buffer)



    def dispatch_raster_splat(self, cmd_list, scene_data, width, height):
        return
        #cmd_list.dispatch(
        #    shader = self.m_raster_splat_shader,
        #    inputs = [
        #        scene_data.metadata_buffer,
        #        scene_data.payload_buffer,
        #        self.m_coarse_tile_list_offsets,
        #        self.m_tile_counter,
        #        self.m_coarse_tile_list_data ],
        #    outputs = self.m_color_buffer,
        #    constants = self.m_constants,
        #    x = utilities.divup(width, 8), y = utilities.divup(height, 8), z = 1)

    def get_coarse_tiles_dims(self, width, height):
        return (int(math.ceil(width/CoarseTileSize)), int(math.ceil(height/CoarseTileSize)))

    def raster(self, cmd_list, scene_data, view_matrix, proj_matrix, width, height):
        (coarse_tile_count_x, coarse_tile_count_y) = self.get_coarse_tiles_dims(width, height)

        self.update_view_resources(width, height, coarse_tile_count_x, coarse_tile_count_y)

        self.clear_view_buffers(cmd_list, width, height, coarse_tile_count_x, coarse_tile_count_y)

        self.update_constants(
            cmd_list,
            view_matrix, proj_matrix, 
            width, height,
            coarse_tile_count_x, coarse_tile_count_y)

        self.dispatch_coarse_tile_bin(cmd_list, scene_data, coarse_tile_count_x, coarse_tile_count_y)

        self.dispatch_raster_splat(cmd_list, scene_data, width, height)
