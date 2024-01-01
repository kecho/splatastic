#include "SceneDb.h"
#include "PlyParser.h"
#include <files/IFileSystem.h>
#include <tasks/ITaskSystem.h>
#include <sstream>

namespace splatastic
{

SceneDb::SceneDb(IFileSystem& fs, ITaskSystem& ts)
: m_fs(fs), m_ts(ts)
{
    for (int i = 0; i < (int)MaxScenes; ++i)
        m_loadStatuses[i] = SceneLoadStatus::Opening;
}

SceneDb::~SceneDb()
{
}

SceneLoadHandle SceneDb::openScene(const char* path)
{
    SceneLoadHandle loadHandle;
    SceneReadState& state = m_loads.allocate(loadHandle);
    if (!loadHandle.valid())
        return SceneLoadHandle();

    std::atomic<SceneLoadStatus>& loadStatus = m_loadStatuses[loadHandle];
    loadStatus = SceneLoadStatus::Reading;
    state.plyFileData = new PlyFileData;
    FileReadRequest readRequest(path, [&state, &loadStatus](FileReadResponse& response)
    {
        if (response.status == FileStatus::Fail)
        {
            std::stringstream ss; 
            ss << "Failed reading file: " << IoError2String(response.error) << std::endl;
            state.errorStr = ss.str();
            loadStatus = SceneLoadStatus::Failed;
        }
        else if (response.status == FileStatus::Reading)
        {
            if (state.plyFileData->errorStr != nullptr)
                return;

            state.bytesRead += response.size;
            state.totalBytes =  response.fileSize;
            parsePlyChunk(*state.plyFileData, response.buffer, response.size);
#if 0
            size_t readOffset = 0;
            if (!state.plyFileData->hasHeader)
                readOffset = parsePlyChunk(*state.plyFileData, response.buffer, response.size);

            if (state.plyFileData->hasHeader)
            {
                if (state.plyFileData->payload == nullptr)
                {
                    state.plyFileData->payloadSize = state.plyFileData->vertexCount * state.plyFileData->strideSize;
                    state.plyFileData->payload = new char[state.plyFileData->payloadSize];
                    state.plyFileData->payloadReadSize = 0;
                }

                size_t chunkSize = response.size - readOffset;
                memcpy(
                    state.plyFileData->payload + state.plyFileData->payloadReadSize,
                    response.buffer + readOffset,
                    chunkSize);
                state.plyFileData->payloadReadSize += chunkSize;
            }
#endif

            loadStatus = SceneLoadStatus::Reading;
        }
        else if (response.status == FileStatus::Success)
        {
            if (state.plyFileData->errorStr != nullptr)
            {
                state.errorStr = state.plyFileData->errorStr;
                loadStatus = SceneLoadStatus::Failed;
            }
            else if (state.plyFileData->payloadSize != state.plyFileData->payloadReadSize)
            {
                std::stringstream ss;
                ss << "Payload of ply file is incomplete: " << state.plyFileData->payloadReadSize << " / " << state.plyFileData->payloadSize;
                state.errorStr = ss.str();
                loadStatus = SceneLoadStatus::Failed;
            }
            else
                loadStatus = SceneLoadStatus::SuccessFinish;
        }
    });

    AsyncFileHandle asyncHandle = m_fs.read(readRequest);
    if (!asyncHandle.valid())
    {
        m_loads.free(loadHandle);
        return SceneLoadHandle();
    }

    m_fs.execute(asyncHandle);
    state.asyncHandle = asyncHandle;
    return loadHandle;
}

SceneLoadStatus SceneDb::checkStatus(SceneLoadHandle handle)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return SceneLoadStatus::InvalidHandle;

    return m_loadStatuses[handle];
}

const char* SceneDb::errorStr(SceneLoadHandle handle)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return "";

    return m_loads[handle].errorStr.c_str();
}

void SceneDb::ioProgress(SceneLoadHandle handle, unsigned long long& bytesRead, unsigned long long& totalBytes)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return;

    SceneReadState& state = m_loads[handle];
    bytesRead = (unsigned long long)state.bytesRead;
    totalBytes = (unsigned long long)state.totalBytes;
}

void SceneDb::resolve(SceneLoadHandle handle)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return;

    SceneReadState& state = m_loads[handle];
    if (state.asyncHandle.valid())
        m_fs.wait(state.asyncHandle);
}

bool SceneDb::copyPayload(SceneLoadHandle handle, char* dest, size_t destSize)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return false;

    SceneReadState& state = m_loads[handle];
    if (!state.asyncHandle.valid())
        return false;

    if (m_loadStatuses[handle] != SceneLoadStatus::SuccessFinish)
        return false;

    if (state.plyFileData == nullptr || state.plyFileData->payload == nullptr)
        return false;

    if (destSize < state.plyFileData->payloadSize)
        return false;

    if (state.copyPayloadTask.valid())
    {
        m_ts.cleanTaskTree(state.copyPayloadTask);
        state.copyPayloadTask = Task();
    }

    PlyFileData* plyData = state.plyFileData;
    TaskDesc td([dest, destSize, plyData, handle, this](TaskContext& ctx)
    {
        memcpy(dest, plyData->payload, plyData->payloadSize);
        m_loadStatuses[handle] = SceneLoadStatus::SuccessFinish;
    });

    state.copyPayloadTask = m_ts.createTask(td);
    if (!state.copyPayloadTask.valid())
        return false;
    
    m_loadStatuses[handle] = SceneLoadStatus::CopyingPayload;
    m_ts.execute(state.copyPayloadTask);
    return true;
}


size_t SceneDb::payloadSize(SceneLoadHandle handle)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return 0;

    const SceneReadState& state = m_loads[handle];
    if (!state.asyncHandle.valid() || state.plyFileData == nullptr)
        return 0ll;

    return state.plyFileData->payloadSize;
}

bool SceneDb::closeScene(SceneLoadHandle handle)
{
    if (!handle.valid() || !m_loads.contains(handle))
        return false;

    SceneReadState& state = m_loads[handle];
    SceneLoadStatus loadStatus = m_loadStatuses[handle];

    if (state.asyncHandle.valid())
        m_fs.closeHandle(state.asyncHandle);

    if (state.copyPayloadTask.valid())
        m_ts.cleanTaskTree(state.copyPayloadTask);

    if (state.plyFileData != nullptr)
    {
        if (state.plyFileData->payload)
            delete [] state.plyFileData->payload;
        delete state.plyFileData;
        state.plyFileData = nullptr;
    }

    m_loadStatuses[handle] = SceneLoadStatus::Opening;
    m_loads.free(handle);
    return true;
}

}
