import os
import sys
import pathlib
import coalpy.gpu
from . import editor
from . import init_module, shutdown_module
from . import overlay
from . import splat_rasterizer

print ("##########################")
print ("####### splatastic #######")
print ("##########################")

init_module()

initial_w = 1600 
initial_h = 900

active_editor = editor.Editor()
active_editor.load_editor_state()

rasterizer = splat_rasterizer.SplatRaster()

def on_render(render_args : coalpy.gpu.RenderArgs):
    if render_args.width == 0 or render_args.height == 0:
        return False
    active_editor.build_ui(render_args.imgui, render_args.implot)
    viewports = active_editor.viewports
    for vp in viewports:
        vp.update(render_args.delta_time)
        cmd_list = coalpy.gpu.CommandList()

        rasterizer.raster(cmd_list, vp.camera.view_matrix, vp.camera.proj_matrix, vp.width, vp.height)
        overlay.render_overlay(cmd_list, rasterizer, vp.texture, vp)
        coalpy.gpu.schedule(cmd_list)
    return

w = coalpy.gpu.Window(
    title="Splatastic - Splatter Renderer",
    on_render = on_render,
    width = initial_w, height = initial_h)

coalpy.gpu.run()
active_editor.save_editor_state()
w = None
shutdown_module()
