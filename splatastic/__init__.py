import os
import sys
import pathlib
import coalpy.gpu
from . import native

g_wave_size = 0
g_module_path = os.path.dirname(pathlib.Path(sys.modules[__name__].__file__)) + "\\"

def _checkGpu(gpuInfo, substring):
    (idx, nm) = gpuInfo
    return substring in nm.lower()

def _queryWaveSize(gpuInfo):
    if _checkGpu(gpuInfo, "nvidia"):
        return 32
    elif _checkGpu(gpuInfo, "amd"):
        return 64
    else:
        return 0

def get_module_path():
    return g_module_path

def get_selected_gpu_wave_size():
    return g_wave_size

def init_module():
    print("Initializing native module.")
    native.init()
    print("Initialization success.")
    print ("Graphics devices:")
    [print("{}: {}".format(idx, nm)) for (idx, nm) in coalpy.gpu.get_adapters()]
    
    #if we find an nvidia or amd gpu, the first one, we select it.
    selected_gpu = next((adapter for adapter in coalpy.gpu.get_adapters() if _checkGpu(adapter, "nvidia") or _checkGpu(adapter, "amd")), None)
    coalpy_settings = coalpy.gpu.get_settings()
    if selected_gpu is not None:
        print ("setting gpu %d" % selected_gpu[0] )
        coalpy_settings.adapter_index = selected_gpu[0]
    
    #coalpy_settings.spirv_debug_reflection = False
    coalpy_settings.graphics_api = "dx12"
    coalpy_settings.enable_debug_device = False
    coalpy.gpu.init()
    coalpy.gpu.add_data_path(g_module_path)
    coalpy.gpu.add_data_path(g_module_path+"/shaders")
    g_wave_size = _queryWaveSize(selected_gpu)
    info = coalpy.gpu.get_current_adapter_info()
    print("device: {}".format(info[1]))

def shutdown_module():
    native.shutdown()
    print("Shutdown.")
