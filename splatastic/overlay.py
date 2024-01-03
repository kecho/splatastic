import coalpy.gpu as g
import math
from . import debug_font

#enums, must match those in debug_cs.hlsl
class OverlayFlags:
    NONE = 0
    SHOW_COARSE_TILES = 1 << 0
    SHOW_FINE_TILES = 1 << 1

#font stuff
g_overlay_shader = None

def init():
    global g_overlay_shader
    g_overlay_shader = g.Shader(file = "shaders/overlay_cs.hlsl", name = "main_overlay", main_function = "csMainOverlay")

def render_overlay(cmd_list, rasterizer, color_buffer, output_texture, view_settings):

    w = view_settings.width
    h = view_settings.height
    (ct_x, ct_y) = rasterizer.get_coarse_tiles_dims(w, h)

    cmd_list.begin_marker("overlay")
    overlay_flags = OverlayFlags.NONE

    cmd_list.dispatch(
        shader = g_overlay_shader,
        constants = [
            int(w), int(h), ct_x, ct_y,
        ],

        inputs = [
            debug_font.font_texture,
            rasterizer.m_tile_counter,
            rasterizer.m_coarse_tile_list_offsets,
            color_buffer
        ],

        samplers = debug_font.font_sampler,

        outputs = output_texture,
        x = math.ceil(w / 32),
        y = math.ceil(h / 32),
        z = 1)
    cmd_list.end_marker()
    
