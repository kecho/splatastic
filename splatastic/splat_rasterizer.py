import coalpy.gpu as g
import math
from . import utilities
from . import camera

CoarseTileSize = 32

class SplatRaster:

    def __init__(self):
        self.m_tile_counter = None
        self.m_constants = None
        self.m_max_width = 0
        self.m_max_height = 0
        self.init_shaders()
        return

    def init_shaders(self):
        self.m_coarse_dispatch_bin_shader = g.Shader(file = "shaders/splat_rasterizer_cs.hlsl", name="CoarseTileBin", main_function = "csCoarseTileBin")

    def update_constants(self, cmd_list, view_matrix, proj_matrix, width, height, coarse_tile_count_x, coarse_tile_count_y):

        constants_data = [
            int(width), int(height), float(1.0/width), float(1.0/height),
            int(coarse_tile_count_x), int(coarse_tile_count_y), float(1.0/coarse_tile_count_x), float(1.0/coarse_tile_count_y),
        ]
        constants_data.extend(view_matrix.flatten().tolist())
        constants_data.extend(proj_matrix.flatten().tolist())

        if self.m_constants == None:
            self.m_constants = g.Buffer(
                name = "SplatRasterConstants",
                stride = 4,
                element_count = len(constants_data),
                usage = g.BufferUsage.Constant)

        cmd_list.upload_resource(source = constants_data, destination =  self.m_constants)

    def update_view_resources(self, width, height, coarse_tile_count_x, coarse_tile_count_y):
        if width <= self.m_max_width and height <= self.m_max_height:
            return

        (self.m_max_width, self.m_max_height) = (width, height)
        self.m_tile_counter = g.Buffer(
            "TileCounter",
            stride = 4, 
            element_count = coarse_tile_count_x * coarse_tile_count_y)
        return

    def clear_view_buffers(self, cmd_list, width, height, coarse_tile_count_x, coarse_tile_count_y):
        utilities.clear_uint_buffer(cmd_list, 0, self.m_tile_counter, 0, coarse_tile_count_x * coarse_tile_count_y)

    def dispatch_coarse_tile_bin(self, cmd_list):
        cmd_list.dispatch(
            shader = self.m_coarse_dispatch_bin_shader,
            constants = self.m_constants,
            outputs = self.m_tile_counter,
            x = 1, y = 1, z = 1)

    def get_coarse_tiles_dims(self, width, height):
        return  (int(math.ceil(width/CoarseTileSize)), int(math.ceil(height/CoarseTileSize)))

    def raster(self, cmd_list, view_matrix, proj_matrix, width, height):
        (coarse_tile_count_x, coarse_tile_count_y) = self.get_coarse_tiles_dims(width, height)

        self.update_view_resources(width, height, coarse_tile_count_x, coarse_tile_count_y)

        self.clear_view_buffers(cmd_list, width, height, coarse_tile_count_x, coarse_tile_count_y)

        self.update_constants(
            cmd_list,
            view_matrix, proj_matrix, 
            width, height,
            coarse_tile_count_x, coarse_tile_count_y)

        self.dispatch_coarse_tile_bin(cmd_list)
