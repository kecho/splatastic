#pragma once

#include <utils/GenericHandle.h>
#include <utils/HandleContainer.h>
#include <tasks/TaskDefs.h>
#include <files/FileDefs.h>
#include <atomic>
#include <string>

namespace splatastic
{

class IFileSystem;

struct SceneLoadHandle : public GenericHandle<int> {};

enum : int { MaxScenes = 8 };

// must match scene_loader.py
enum class SceneLoadStatus
{
    Opening,
    Reading,
    CopyingPayload,
    InvalidHandle,
    SuccessFinish,
    Failed
};

struct PlyFileData;

struct SplatSceneMetadata
{
    size_t vertexCount;
    size_t stride;
};

class SceneDb
{
public:
    SceneDb(IFileSystem& fs, ITaskSystem& ts);
    ~SceneDb();

    SceneLoadHandle openScene(const char* path);
    void openScenePayload(SceneLoadHandle sceneHandle);
    SceneLoadStatus checkStatus(SceneLoadHandle handle);
    size_t payloadSize(SceneLoadHandle sceneHandle);

    void ioProgress(
        SceneLoadHandle handle,
        unsigned long long& bytesRead,
        unsigned long long& totalBytes);

    bool copyPayload(SceneLoadHandle handle, char* dest, size_t destSize);

    bool sceneMetadata(SceneLoadHandle handle, SplatSceneMetadata& metadata);

    const char* errorStr(SceneLoadHandle handle);
    void resolve(SceneLoadHandle handle);

    bool closeScene(SceneLoadHandle handle);

private:
    struct SceneReadState
    {
        AsyncFileHandle asyncHandle = {};
        Task copyPayloadTask = {};
        std::string errorStr = {};
        size_t bytesRead = 0;
        size_t totalBytes = 0;
        PlyFileData* plyFileData = {};
    };

    HandleContainer<SceneLoadHandle, SceneReadState, MaxScenes> m_loads;
    std::atomic<SceneLoadStatus> m_loadStatuses[MaxScenes];
    IFileSystem& m_fs;
    ITaskSystem& m_ts;
};

}
