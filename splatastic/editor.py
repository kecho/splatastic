import coalpy.gpu as g
import coalpy.utils
import numpy as np
import inspect
import os.path
import sys
import pathlib
import json
import math
from . import native
from . import scene_loader
from . import get_module_path
from . import camera as c
from . import transform as t
from . import vec


class EditorPanel:
    def __init__(self, name, state):
        self.name = name
        self.state = state

class EditorViewport:

    def __init__(self, id):
        self.m_name = "Viewport " + str(id)
        self.m_texture = None
        self.m_width = 1920
        self.m_height = 1080
        self.m_active = True
        self.m_is_focused = False
        self.m_id = id

        #camera data
        self.m_editor_camera = c.Camera(1920, 1080)
        self.reset_camera()

        #input state
        self.m_right_pressed = False
        self.m_left_pressed = False
        self.m_top_pressed = False
        self.m_bottom_pressed = False
        self.m_can_move_pressed = False
        self.m_can_orbit_pressed = False
        self.m_last_mouse = (0.0, 0.0)

        #camera settings
        self.m_cam_move_speed = 4.0
        self.m_cam_rotation_speed = 0.1
        self.m_last_mouse = (0, 0)
        self.m_curr_mouse = (0, 0)

        # public debug info
        self.gpu_view_debug_info = None
        self.request_gpu_view_debug_info = False

    def save_editor_state(self):
        return {
            'id' : self.m_id,
            'name' : self.m_name
        }

    def load_editor_state(self, json):
        self.m_id = json['id']
        self.m_name = json['name']

    def build_ui(self, imgui: g.ImguiBuilder):
        self.m_active = imgui.begin(self.m_name, self.m_active)
        (cr_min_w, cr_min_h) = imgui.get_window_content_region_min()
        (cr_max_w, cr_max_h) = imgui.get_window_content_region_max()
        (nw, nh) = (int(cr_max_w - cr_min_w), int(cr_max_h - cr_min_h))
        self.m_is_focused = imgui.is_window_focused(flags = g.ImGuiFocusedFlags.RootWindow)
        if (self.m_active):
            self._update_inputs(imgui)
            #update viewport texture
            if (nw > 0 and nh > 0 and (self.m_texture == None or self.m_width != nw or self.m_height != nh)):
                self.m_width = nw;
                self.m_height = nh;
                self.m_texture = g.Texture(
                    name = self.m_name, width = self.m_width, height = self.m_height,
                    format = g.Format.RGBA_8_UNORM)

            if (self.m_texture != None):
                imgui.image(
                    texture = self.m_texture,
                    size = (self.m_width, self.m_height))
        imgui.end() 
        return self.m_active

    def reset_camera(self):
        initial_pos = vec.float3(0, 0, -20)
        cam = self.m_editor_camera
        cam.pos = initial_pos
        cam.rotation = vec.q_from_angle_axis(0, vec.float3(1, 0, 0))
        cam.focus_distance = vec.veclen(initial_pos)
        cam.fov = 20 * t.to_radians()
        cam.near = 0.01
        cam.far = 10000
        cam.update_mats()

    def _rotate_transform_mouse_control(self, target_transform, curr_mouse, delta_time, x_axis_sign = 1.0, y_axis_sign = 1.0):
        rot_vec = delta_time * self.m_cam_rotation_speed * vec.float3(curr_mouse[0] - self.m_last_mouse[0], curr_mouse[1] - self.m_last_mouse[1], 0.0)
        y_axis = vec.float3(0, 1, 0)
        qx = vec.q_from_angle_axis(-np.sign(x_axis_sign * rot_vec[0]) * (np.abs(rot_vec[0]) ** 1.2), y_axis)
        target_transform.rotation = (qx * target_transform.rotation)
        
        x_axis = target_transform.right
        qy = vec.q_from_angle_axis(np.sign(y_axis_sign * rot_vec[1]) * (np.abs(rot_vec[1]) ** 1.2), x_axis)
        target_transform.rotation = (qy * target_transform.rotation)

    def _get_rel_mouse(self, imgui: g.ImguiBuilder):
        if self.m_width == 0 or self.m_height == 0:
            return (0, 0)
        (ax, ay) = imgui.get_mouse_pos()
        (wx, wy) = imgui.get_cursor_screen_pos()
        return (((ax - wx) + 0.5)/self.m_width, ((ay - wy) + 0.5)/self.m_height)

    def _update_inputs(self, imgui : g.ImguiBuilder):
        curr_mouse_pos = self._get_rel_mouse(imgui)
        is_right_click = imgui.is_mouse_down(g.ImGuiMouseButton.Right)

        #if is_right_click and curr_mouse_pos[0] >= 0.0 and curr_mouse_pos[0] <= 1.0 and curr_mouse_pos[1] >= 0.0 and curr_mouse_pos[1] <= 1.0:
        if is_right_click and imgui.is_window_hovered():
            imgui.set_window_focus()

        if not self.m_is_focused:
            self.m_can_move_pressed = False
            self.m_can_orbit_pressed = False
            return

        self.m_right_pressed = imgui.is_key_down(g.ImGuiKey.D)
        self.m_left_pressed = imgui.is_key_down(g.ImGuiKey.A)
        self.m_top_pressed = imgui.is_key_down(g.ImGuiKey.W)
        self.m_bottom_pressed = imgui.is_key_down(g.ImGuiKey.S)
        prev_move_pressed = self.m_can_move_pressed
        prev_orbit_pressed = self.m_can_orbit_pressed
        self.m_can_move_pressed =  is_right_click
        self.m_can_orbit_pressed =  imgui.is_key_down(g.ImGuiKey.LeftAlt) and imgui.is_mouse_down(g.ImGuiMouseButton.Left)
        if prev_move_pressed != self.m_can_move_pressed or prev_orbit_pressed != self.m_can_orbit_pressed:
            self.m_curr_mouse = curr_mouse_pos
            self.m_last_mouse = self.m_curr_mouse

        if self.m_can_move_pressed or self.m_can_orbit_pressed:
            self.m_last_mouse = self.m_curr_mouse
            self.m_curr_mouse = curr_mouse_pos

    def update(self, delta_time, rasterizer):
        if self.request_gpu_view_debug_info:
            self.gpu_view_debug_info = rasterizer.update_gpu_debug_view_info(self.gpu_view_debug_info)
        
        self.m_editor_camera.w = self.m_width
        self.m_editor_camera.h = self.m_height
        if (self.m_can_move_pressed):
            new_pos = self.m_editor_camera.pos
            zero = vec.float3(0, 0, 0)
            cam_transform = self.m_editor_camera.transform
            new_pos = new_pos - ((cam_transform.right * self.m_cam_move_speed) if self.m_right_pressed  else zero)
            new_pos = new_pos + ((cam_transform.right * self.m_cam_move_speed) if self.m_left_pressed   else zero)
            new_pos = new_pos + ((cam_transform.front * self.m_cam_move_speed   ) if self.m_top_pressed    else zero)
            new_pos = new_pos - ((cam_transform.front * self.m_cam_move_speed   ) if self.m_bottom_pressed else zero)
            self.m_editor_camera.pos = new_pos
            self._rotate_transform_mouse_control(cam_transform, self.m_curr_mouse, delta_time)
            self.m_last_mouse = (self.m_curr_mouse[0], self.m_curr_mouse[1])
        elif (self.m_can_orbit_pressed):
            lookat_pos = self.m_editor_camera.focus_point
            lookat_dist = self.m_editor_camera.focus_distance
            cam_transform = self.m_editor_camera.transform
            self._rotate_transform_mouse_control(cam_transform, self.m_curr_mouse, delta_time, -1.0)
            cam_transform.translation = lookat_pos - lookat_dist * cam_transform.front
            cam_transform.update_mats()
            self.m_last_mouse = (self.m_curr_mouse[0], self.m_curr_mouse[1])
            
    @property
    def camera(self):
        return self.m_editor_camera

    @property
    def width(self):
        return self.m_width

    @property
    def height(self):
        return self.m_height
    
    @property
    def texture(self):
        return self.m_texture

    @property
    def id(self):
        return self.m_id

    @property
    def name(self):
        return self.m_name

    @property
    def is_focused(self):
        return self.m_is_focused

    @property
    def debug_fine_tiles(self):
        return self.m_debug_fine_tiles

    @debug_fine_tiles.setter
    def debug_fine_tiles(self, value):
        self.m_debug_fine_tiles = value
    
class Editor:
    
    def __init__(self):
        self.m_active_scene_name = None
        self.m_active_scene = None
        self.m_set_default_layout = False
        self.m_ui_frame_it = 0
        self.m_selected_viewport = None
        self.m_viewports = {}

        self.m_open_active = False
        self.m_tools = self.createToolPanels()

        #profiler
        self.m_profiler = None

        #scene data
        self.m_scene_loader = None
        self.m_scene_data = None

    @property
    def scene_data(self):
        return self.m_scene_data

    def createToolPanels(self):
        return {
            'view_panel' : EditorPanel("View Settings", False),
            'scene_panel' : EditorPanel("Scene", False),
            'profiler' : EditorPanel("Profiler", False)
        }

    def save_editor_state(self):
        state = {
            'tools_states' : [(k, v.state) for (k, v) in self.m_tools.items()],
            'viewport_states' : [vp.save_editor_state() for vp in self.m_viewports.values()]
        }
        try:
            f = open('editor_state.json', "w")
            f.write(json.dumps(state))
            f.close()
        except Exception as err:
            print("[Editor]: error saving state"+str(err))

    def load_editor_state(self):
        try:
            if not os.path.exists('editor_state.json'):
                return

            f = open('editor_state.json', "r")

            state = json.loads(f.read())
            if 'tools_states' in state:
                toolsTuples = state['tools_states']
                for (tn, tstate) in toolsTuples:
                    if tn in self.m_tools:
                        self.m_tools[tn].state = tstate
            if 'viewport_states' in state:
                for vp_json in state['viewport_states']:
                    new_vp = EditorViewport(0)
                    new_vp.load_editor_state(vp_json)
                    self.m_viewports[new_vp.id] = new_vp
            f.close()
        except Exception as err:
            print("[Editor]: error loading state"+str(err))

    def build_menu_bar(self, imgui : g.ImguiBuilder):
        if (imgui.begin_main_menu_bar()):
            if (imgui.begin_menu("File")):
                if (imgui.menu_item("Open Scene")):
                    self.m_open_active = self.m_scene_loader is None
                imgui.end_menu()
            if (imgui.begin_menu("Tools")):
                for t in self.m_tools.values():
                    t.state = True if imgui.menu_item(label = t.name) else t.state
                imgui.end_menu()
            if (imgui.begin_menu("Window")):
                if (imgui.menu_item(label = "New Viewport")):
                    vp_id_list = [vp.id for vp in self.m_viewports.values()]
                    next_id = (0 if len(vp_id_list) == 0 else (max(vp_id_list) + 1))
                    new_name = "Viewport " + str(next_id)
                    self.m_viewports[next_id] = EditorViewport(next_id)
                if (imgui.menu_item(label = "Reset Layout")):
                    self.m_set_default_layout = True
                imgui.end_menu()
            imgui.end_main_menu_bar()

    def build_view_settings_panel(self, imgui : g.ImguiBuilder):
        panel = self.m_tools['view_panel']
        if not panel.state:
            return

        panel.state = imgui.begin(panel.name, panel.state)
        if self.m_selected_viewport != None:
            if (imgui.collapsing_header("Camera", g.ImGuiTreeNodeFlags.DefaultOpen)):
                cam = self.m_selected_viewport.camera
                imgui.text(self.m_selected_viewport.name)
                cam.fov = imgui.slider_float(label="fov", v=cam.fov, v_min=0.01 * np.pi, v_max=0.7 * np.pi)
                cam.near = imgui.slider_float(label="near", v=cam.near, v_min=0.001, v_max=8.0)
                cam.far = imgui.slider_float(label="far", v=cam.far, v_min=10.0, v_max=90000)

                nx = cam.transform.translation[0]
                ny = cam.transform.translation[1]
                nz = cam.transform.translation[2]
                (nx, ny, nz) = imgui.input_float3(label="pos", v=[nx, ny, nz])
                cam.transform.translation = [nx, ny, nz]
                if (imgui.button("reset")):
                    self.m_selected_viewport.reset_camera()

                self.m_selected_viewport.m_cam_move_speed = imgui.slider_float(label="moving speed", v = self.m_selected_viewport.m_cam_move_speed, v_min = 0.01, v_max = 16.0)

            self.m_selected_viewport.request_gpu_view_debug_info = imgui.collapsing_header("Raster Debug Info")
            if (self.m_selected_viewport.request_gpu_view_debug_info):
                gpu_debug_info = self.m_selected_viewport.gpu_view_debug_info
                if gpu_debug_info != None:
                    imgui.text("Max tile records: %d " % gpu_debug_info.coarse_tile_record_max)
                    imgui.text("View tile record count: %d " % gpu_debug_info.current_view_tile_records)
                    prog_bar_fraction = 0 if gpu_debug_info.coarse_tile_record_max == 0 else gpu_debug_info.current_view_tile_records / gpu_debug_info.coarse_tile_record_max
                    imgui.progress_bar(
                        fraction = prog_bar_fraction, overlay = "%d" % (int(100 * prog_bar_fraction)))
                else:
                    imgui.text("Loading debug info...")
            else:
                self.m_selected_viewport.gpu_view_debug_info = None

        imgui.end()

    def build_profiler(self, imgui : g.ImguiBuilder, implot : g.ImplotBuilder):
        panel = self.m_tools['profiler']
        if not panel.state:
            self.m_profiler = None
            return

        if self.m_profiler is None:
            self.m_profiler = coalpy.utils.Profiler()

        panel.state = imgui.begin(panel.name, panel.state)
        self.m_profiler.build_ui(imgui, implot)
        imgui.end()

    def build_scene_panel(self, imgui : g.ImguiBuilder):
        panel = self.m_tools['scene_panel']
        if not panel.state:
            return

        panel.state = imgui.begin(panel.name, panel.state)

        if self.m_scene_loader is None and imgui.button("Open Scene") and not self.m_open_active:
            self.m_open_active = True
        elif self.m_scene_loader != None:
            imgui.text("Opening scene...")
            (status, percentage, msg) = self.m_scene_loader.update_load_status()
            if status == scene_loader.Reading:
                imgui.progress_bar(fraction = percentage)
            elif status == scene_loader.Failed:
                print("Failed opening scene, reason: "+ msg) 
                self.m_scene_loader = None
            elif status == scene_loader.SuccessFinish:
                print("Success loading scene.") 
                self.m_scene_data = self.m_scene_loader.scene_data
                self.m_scene_loader = None
                if self.m_scene_data.vertex_count == 0:
                    print ("No vertex count found. Unloading scene.")
                    self.m_scene_data.vertex_count = None
        
        if self.m_scene_data != None:
            imgui.text("Scene vertices: %d" % self.m_scene_data.vertex_count)
            imgui.text("Vertex stride: %d" % self.m_scene_data.stride)

        imgui.end()

    @property
    def viewports(self):
        return self.m_viewports.values()

    @property
    def profiler(self):
        return self.m_profiler

    def setup_default_layout(self, root_d_id, imgui : g.ImguiBuilder):
        settings_loaded = imgui.settings_loaded()
        if ((settings_loaded or self.m_ui_frame_it > 0) and not self.m_set_default_layout):
            return

        if 0 not in self.m_viewports:
            newVp = EditorViewport(0)
            self.m_viewports[0] = newVp

        imgui.dockbuilder_remove_child_nodes(root_d_id)
        (t, l, r) = imgui.dockbuilder_split_node(node_id=root_d_id, split_dir = g.ImGuiDir.Left, split_ratio = 0.2)
        imgui.dockbuilder_dock_window("Viewport 0", r)

        (t, l, r) = imgui.dockbuilder_split_node(node_id=t, split_dir = g.ImGuiDir.Down, split_ratio = 0.5)
        view_panel = self.m_tools['view_panel']
        scene_panel = self.m_tools['scene_panel']
        view_panel.state = True
        scene_panel.state = True
        imgui.dockbuilder_dock_window(view_panel.name, t)
        imgui.dockbuilder_dock_window(scene_panel.name, r)
        imgui.dockbuilder_finish(root_d_id)
        self.m_set_default_layout = False

    def load_scene(self, file_path_name):
        if file_path_name is None:
            return
        self.m_scene_loader = scene_loader.Loader(file_path_name)

    def build_open_file(self, imgui):
        if not self.m_open_active:
            return

        imgui.set_next_window_size((600.0, 400.0), g.ImGuiCond.FirstUseEver)
        ret = imgui.open_file_dialog("fopen", "Open Scene", ".ply", ".")
        if ret is None:
            return

        self.m_open_active = False
        (cmd, file_path_name, file_path) = ret
        if cmd == "ok":
            self.load_scene(file_path_name)

    def build_ui(self, imgui : g.ImguiBuilder, implot : g.ImplotBuilder):
        root_d_id = imgui.get_id("RootDock")
        imgui.begin(name="MainWindow", is_fullscreen = True)
        imgui.dockspace(dock_id=root_d_id)
        imgui.end()

        self.build_menu_bar(imgui)
        self.build_view_settings_panel(imgui)
        self.build_scene_panel(imgui)
        self.build_open_file(imgui)
        self.build_profiler(imgui, implot)

        viewport_objs = [vo for vo in self.m_viewports.values()]
        for vp in viewport_objs: 
            if not vp.build_ui(imgui):
                if self.m_selected_viewport is not None and vp is self.m_selected_viewport:
                    self.m_selected_viewport = None
                del self.m_viewports[vp.id]

        svp = next((x for x in self.viewports if x.is_focused), None)
        self.m_selected_viewport = self.m_selected_viewport if svp is None else svp
        self.setup_default_layout(root_d_id, imgui)
        self.m_ui_frame_it = self.m_ui_frame_it + 1
        return

    def profiler_begin_capture(self):
        if self.m_profiler is None:
            return
        self.m_profiler.begin_capture()

    def profiler_end_capture(self):
        if self.m_profiler is None:
            return
        self.m_profiler.end_capture()

