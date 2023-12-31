#include "FileSystem.h"
#include <tasks/ITaskSystem.h>
#include <utils/Assert.h>
#include "Utils.h"
#include <sstream>

namespace splatastic
{

FileSystem::FileSystem(const FileSystemDesc& desc)
: m_desc(desc)
, m_ts(*desc.taskSystem)
{
    
}

FileSystem::~FileSystem()
{
    SPT_ASSERT_FMT(m_requests.elementsCount() == 0, "%d File requests still alive. Please close the handles.", m_requests.elementsCount());
}

AsyncFileHandle FileSystem::read(const FileReadRequest& request)
{
    SPT_ASSERT_MSG(request.doneCallback, "File read request must provide a done callback.");
    AsyncFileHandle asyncHandle;
    Task task;
    
    {
        std::unique_lock lock(m_requestsMutex);
        Request*& requestData = m_requests.allocate(asyncHandle);
        requestData = new Request();
        requestData->type = InternalFileSystem::RequestType::Read;
        requestData->filenames.push(request.path);
        {
            for (auto& root : request.additionalRoots)
            {
                if (root[root.size() - 1] == '/' || root[root.size() - 1] == '\\')
                    requestData->filenames.push(root + request.path);
                else
                    requestData->filenames.push(root + FILE_SEP + request.path);
            }
        }

        requestData->readCallback = request.doneCallback;
        requestData->opaqueHandle = {};
        requestData->error = IoError::None;
        requestData->fileStatus = FileStatus::Idle;
        requestData->task = m_ts.createTask(TaskDesc("FileSystem::read", [this](TaskContext& ctx)
        {
            auto* requestData = (Request*)ctx.data;
            {
                requestData->fileStatus = FileStatus::Opening;
                FileReadResponse response;
                response.status = FileStatus::Opening;
                requestData->readCallback(response);
            }

            //pop all the candidate files that don't exist
            while(!requestData->filenames.empty())
            {
                FileAttributes attr;
                getFileAttributes(requestData->filenames.front().c_str(), attr);
                if (!attr.exists || attr.isDir || attr.isDot)
                    requestData->filenames.pop();
                else
                    break;
            } 
    
            if (!requestData->filenames.empty())
                requestData->opaqueHandle = InternalFileSystem::openFile(requestData->filenames.front().c_str(), InternalFileSystem::RequestType::Read);

            if (!InternalFileSystem::valid(requestData->opaqueHandle))
            {
                {
                    requestData->error = IoError::FailedOpening;
                    requestData->fileStatus = FileStatus::Fail;
                    FileReadResponse response;
                    if (!requestData->filenames.empty())
                        response.filePath = requestData->filenames.front();
                    response.error = IoError::FailedOpening;
                    response.status = FileStatus::Fail;
                    requestData->readCallback(response);
                }
                return;
            }

            std::string resolvedFileName;
            FileUtils::getAbsolutePath(requestData->filenames.front(), resolvedFileName);

            requestData->fileStatus = FileStatus::Reading;

            size_t fileSize = InternalFileSystem::fileSize(requestData->opaqueHandle);

            struct ReadState {
                char* output = nullptr;
                int bytesRead = 0;
                bool isEof = false;
                bool successRead = false;
            } readState;
            while (!readState.isEof)
            {
                TaskUtil::yieldUntil([&readState, requestData]() {
                    readState.successRead = InternalFileSystem::readBytes(
                        requestData->opaqueHandle, readState.output, readState.bytesRead, readState.isEof);
                });

                {
                    FileReadResponse response;
                    response.status = FileStatus::Reading;
                    response.buffer = readState.output;
                    response.fileSize = fileSize;
                    response.size = readState.bytesRead;
                    response.filePath = resolvedFileName;
                    requestData->readCallback(response);
                }

                if (!readState.successRead)
                {
                    {
                        if (InternalFileSystem::valid(requestData->opaqueHandle))
                            InternalFileSystem::close(requestData->opaqueHandle);

                        requestData->error = IoError::FailedReading;
                        requestData->fileStatus = FileStatus::Fail;
                        FileReadResponse response;
                        response.error = IoError::FailedReading;
                        response.filePath = resolvedFileName;
                        response.status = FileStatus::Fail;
                        requestData->readCallback(response);
                    }
                    return;
                }
            }

            {
                if (InternalFileSystem::valid(requestData->opaqueHandle))
                    InternalFileSystem::close(requestData->opaqueHandle);

                requestData->fileStatus = FileStatus::Success;
                FileReadResponse response;
                response.filePath = resolvedFileName;
                response.status = FileStatus::Success;
                requestData->readCallback(response);
            }

        }), requestData);
        task = requestData->task;
    }

    if ((request.flags & (int)FileRequestFlags::AutoStart) != 0)
        m_ts.execute(task);
    return asyncHandle;
}

void FileSystem::execute(AsyncFileHandle handle)
{
    Task task = asTask(handle);
    m_ts.execute(task);
}

Task FileSystem::asTask(AsyncFileHandle handle)
{
    Task task;
    {
        std::unique_lock lock(m_requestsMutex);
        SPT_ASSERT(m_requests.contains(handle));
        Request* requestData = m_requests[handle];
        task = requestData->task;
    }
    return task;
}

AsyncFileHandle FileSystem::write(const FileWriteRequest& request)
{
    SPT_ASSERT_MSG(request.doneCallback, "File read request must provide a done callback.");

    AsyncFileHandle asyncHandle;
    Task task;
    
    {
        std::unique_lock lock(m_requestsMutex);
        Request*& requestData = m_requests.allocate(asyncHandle);
        requestData = new Request();
        requestData->type = InternalFileSystem::RequestType::Write;
        requestData->filenames.push(request.path);
        InternalFileSystem::fixStringPath(requestData->filenames.front());
        requestData->writeCallback = request.doneCallback;
        requestData->opaqueHandle = {};
        requestData->error = IoError::None;
        requestData->fileStatus = FileStatus::Idle;
        requestData->writeBuffer.append((const u8*)request.buffer, (size_t)request.size);
        requestData->writeSize = request.size;
        requestData->task = m_ts.createTask(TaskDesc([this](TaskContext& ctx)
        {
            auto* requestData = (Request*)ctx.data;
            {
                requestData->fileStatus = FileStatus::Opening;
                FileWriteResponse response;
                response.status = FileStatus::Opening;
                requestData->writeCallback(response);
            }

            if (!InternalFileSystem::carvePath(requestData->filenames.front()))
            {
                requestData->error = IoError::FailedCreatingDir;
                requestData->fileStatus = FileStatus::Fail;
                FileWriteResponse response;
                response.error = IoError::FailedCreatingDir;
                response.status = FileStatus::Fail;
                requestData->writeCallback(response);
                {
                    FileWriteResponse response;
                    response.status = FileStatus::Fail;
                    response.error = IoError::FailedCreatingDir;
                    requestData->writeCallback(response);
                }
                return;
            }
            requestData->opaqueHandle = InternalFileSystem::openFile(requestData->filenames.front().c_str(), InternalFileSystem::RequestType::Write);
            if (!InternalFileSystem::valid(requestData->opaqueHandle))
            {
                {
                    requestData->error = IoError::FailedOpening;
                    requestData->fileStatus = FileStatus::Fail;
                    FileWriteResponse response;
                    response.error = IoError::FailedOpening;
                    response.status = FileStatus::Fail;
                    requestData->writeCallback(response);
                }
                return;
            }

            requestData->fileStatus = FileStatus::Writing;

            struct WriteState {
                const char* buffer;
                int bufferSize;
                bool successWrite;
            } writeState = { (const char*)requestData->writeBuffer.data(), requestData->writeSize, false };

            TaskUtil::yieldUntil([&writeState, requestData]() {
                writeState.successWrite = InternalFileSystem::writeBytes(
                    requestData->opaqueHandle, writeState.buffer, writeState.bufferSize);
            });

            if (!writeState.successWrite)
            {
                {
                    if (InternalFileSystem::valid(requestData->opaqueHandle))
                        InternalFileSystem::close(requestData->opaqueHandle);

                    requestData->error = IoError::FailedWriting;
                    requestData->fileStatus = FileStatus::Fail;
                    FileWriteResponse response;
                    response.error = IoError::FailedWriting;
                    response.status = FileStatus::Fail;
                    requestData->writeCallback(response);
                }
                return;
            }

            {
                if (InternalFileSystem::valid(requestData->opaqueHandle))
                    InternalFileSystem::close(requestData->opaqueHandle);

                requestData->fileStatus = FileStatus::Success;
                FileWriteResponse response;
                response.status = FileStatus::Success;
                requestData->writeCallback(response);
            }

        }), requestData);
        task = requestData->task;
    }

    if ((request.flags & (int)FileRequestFlags::AutoStart) != 0)
        m_ts.execute(task);
    return asyncHandle;
}


void FileSystem::wait(AsyncFileHandle handle)
{
    Task task;
    {
        std::unique_lock lock(m_requestsMutex);
        Request* r = m_requests[handle];
        task = r->task;
    }

    m_ts.wait(task);
}

bool FileSystem::readStatus (AsyncFileHandle handle, FileReadResponse& response)
{
    return false;
}

bool FileSystem::writeStatus(AsyncFileHandle handle, FileWriteResponse& response)
{
    return false;
}

void FileSystem::closeHandle(AsyncFileHandle handle)
{
    Request* requestData = nullptr;
    {
        std::unique_lock lock(m_requestsMutex);
        if (!m_requests.contains(handle))
            return;

        requestData = m_requests[handle];
        if (requestData == nullptr)
            return;
    }

    m_ts.wait(requestData->task);
    m_ts.cleanTaskTree(requestData->task);

    if (InternalFileSystem::valid(requestData->opaqueHandle))
        InternalFileSystem::close(requestData->opaqueHandle);

    delete requestData;
    
    {
        std::unique_lock lock(m_requestsMutex);
        m_requests.free(handle);
    }
}

bool FileSystem::carveDirectoryPath(const char* directoryName)
{
    std::string dir = directoryName;
    InternalFileSystem::fixStringPath(dir);
    return InternalFileSystem::carvePath(dir, false);
}

void FileSystem::enumerateFiles(const char* directoryName, std::vector<std::string>& dirList)
{
    std::string dirName = directoryName;
    InternalFileSystem::enumerateFiles(dirName, dirList);
}

bool FileSystem::deleteDirectory(const char* directoryName)
{
    return InternalFileSystem::deleteDirectory(directoryName);
}

bool FileSystem::deleteFile(const char* fileName)
{
    return InternalFileSystem::deleteFile(fileName);
}

void FileSystem::getFileAttributes(const char* fileName, FileAttributes& attributes)
{
    InternalFileSystem::getAttributes(fileName, attributes.exists, attributes.isDir, attributes.isDot);
}

IFileSystem* IFileSystem::create(const FileSystemDesc& desc)
{
    return new FileSystem(desc);
}

}
