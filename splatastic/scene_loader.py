from . import native as n
import coalpy.gpu

# must match SceneDb.h enums
Opening = 0
Reading = 1
CopyingPayload = 2
InvalidHandle = 3
SuccessFinish = 4
Failed = 5

class Loader:
    def __init__(self, file_name):
        self.m_request = n.SceneAsyncRequest(file = file_name)
        self.m_gpu_upload_buffer = None
        self.m_payload_buffer = None
        self.m_payload_ready = False

    def update_load_status(self):
        if self.m_payload_ready:
            return (SuccessFinish, 1.0, "Success")

        if self.m_request == None:
            return (Opening, 0.0, "")

        (status, msg) = self.m_request.status()
        if status == Reading:
            (bytes_read, total_bytes) = self.m_request.ioProgress()
            return (Reading, 0.0 if total_bytes == 0 else bytes_read/total_bytes, msg)
        elif status == Failed:
            return (Failed, 0.0, msg)
        elif status == CopyingPayload:
            return (Reading, 1.0, "Copying payload to GPU write combined")
        elif status == SuccessFinish:
            if self.m_gpu_upload_buffer is None:
                payload_size = self.m_request.payload_size()
                if payload_size == 0:
                    return (Failed, 0.0, "Payload size recovered from scene is 0")

                self.m_gpu_upload_buffer = coalpy.gpu.Buffer(
                    name="TmpWriteCombined",
                    format = coalpy.gpu.Format.R32_UINT,
                    stride = 4,
                    element_count = int((payload_size + 3)/4),
                    mem_flags = coalpy.gpu.MemFlags.GpuRead,
                    usage = coalpy.gpu.BufferUsage.Upload)

                self.m_request.request_copy_payload(self.m_gpu_upload_buffer.mappedMemory())
                self.m_payload_buffer = coalpy.gpu.Buffer(
                    name="ScenePayloadBuffer",
                    type = coalpy.gpu.BufferType.Raw,
                    stride = 4,
                    element_count = int((payload_size + 3)/4),
                    mem_flags = coalpy.gpu.MemFlags.GpuRead | coalpy.gpu.MemFlags.GpuWrite)
                return (Reading, 1.0, "")
            else:
                self.m_request.close_copy_payload()
                self.m_request = None
                cmd_list = coalpy.gpu.CommandList()
                cmd_list.copy_resource(self.m_gpu_upload_buffer, self.m_payload_buffer)
                coalpy.gpu.schedule(cmd_list)
                self.m_gpu_upload_buffer = None
                self.m_payload_ready = True
                return (SuccessFinish, 1.0, "Success")

        return (Failed, 0.0, "Unknown state")
