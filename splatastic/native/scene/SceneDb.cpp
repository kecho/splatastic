#include "SceneDb.h"
#include <utils/ClTokenizer.h>
#include <files/IFileSystem.h>
#include <tasks/ITaskSystem.h>
#include <sstream>

namespace splatastic
{

struct PlyFileData
{
    const char* errorStr = nullptr;
    bool hasHeader = false;
    int vertexCount = 0;
    int strideSize = 0;
    size_t payloadReadSize = 0;
    size_t payloadSize = 0;
    char* payload = nullptr;
};

namespace 
{

struct Token
{
    const char* data;
    int size;
};

bool isToken(Token tok, const char* buffer, int bufferSize)
{
    if (bufferSize < tok.size)
        return false;

    for (int i = 0; i < tok.size; ++i)
        if (buffer[i] != tok.data[i])
            return false;

    return true;
}

void nextWord(const char* buffer, int bufferSize, int& wordBegin, int& wordEnd)
{
    for (wordBegin = wordEnd; wordBegin < bufferSize && (buffer[wordBegin] == ' ' || buffer[wordBegin] == '\t'); ++wordBegin);
    for (wordEnd = wordBegin; wordEnd < bufferSize && buffer[wordEnd] != ' ' && buffer[wordEnd] != '\t' && buffer[wordEnd] != '\n'; ++wordEnd);
}

size_t parsePlyHeader(PlyFileData& fileData, const char* buffer, size_t bufferSize)
{
    fileData.errorStr = nullptr;

    enum ReadState
    {
        BeginHeader, HeaderContent, EndHeader
    };

    const Token plyToken = { "ply", 3 };
    const Token v1_0Token = { "1.0", 3 };
    const Token binaryLittleIndianToken = { "binary_little_endian", 20 };
    const Token propertyToken = { "property", 8 };
    const Token floatToken = { "float", 5 };
    const Token elementToken = { "element", 7 };
    const Token vertexToken = { "vertex", 6 };
    const Token formatToken = { "format", 6 };
    const Token endHeaderToken = { "end_header", 10 };

    ReadState readState = BeginHeader;
    size_t offset = 0;
    const char* endBuffer = buffer + bufferSize;
    const int maxLines = 1000;
    int lineIndex = 0;
    while (offset < bufferSize)
    {
        //count characters for new line.
        const char* lineBuffer = buffer + offset;
        int lineSize = 0;
        for (; (lineBuffer + lineSize) < endBuffer && lineBuffer[lineSize] != '\n'; ++lineSize);
        
        //we have a new line we can tokenize
        if (readState == BeginHeader)
        {
            if (!isToken(plyToken, lineBuffer, lineSize))
            {
                fileData.errorStr = "Expecting ply token at the top of the ply file.";
                return offset;
            }

            readState = HeaderContent;
        }
        else if (readState == HeaderContent)
        {
            if (isToken(endHeaderToken, lineBuffer, lineSize))
            {
                readState = EndHeader;
            }
            else
            {
                int wordBegin = 0, wordEnd = 0;
                nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                if (isToken(propertyToken, lineBuffer + wordBegin, wordEnd - wordBegin))
                {
                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                    if (isToken(floatToken, lineBuffer + wordBegin, wordEnd - wordBegin))
                    {
                        fileData.strideSize += 4; //4 bytes
                    }
                    else
                    {
                        fileData.errorStr = "Only supports float property";
                        return offset;
                    }

                    //skip the name of the property
                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                }
                else if (isToken(formatToken, lineBuffer + wordBegin, wordEnd - wordBegin))
                {
                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                    if (!isToken(binaryLittleIndianToken, lineBuffer + wordBegin, wordEnd - wordBegin))
                    {
                        fileData.errorStr = "Only supports binary little endian type";
                        return offset;
                    }

                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                    if (!isToken(v1_0Token, lineBuffer + wordBegin, wordEnd - wordBegin))
                    {
                        fileData.errorStr = "Only supports binary little endian version 1.0";
                        return offset;
                    }
                }
                else if (isToken(elementToken, lineBuffer + wordBegin, wordEnd - wordBegin)) 
                {
                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                    if (!isToken(vertexToken, lineBuffer + wordBegin, wordEnd - wordBegin))
                    {
                        fileData.errorStr = "Only supports vertex token type";
                        return offset;
                    }

                    nextWord(lineBuffer, lineSize, wordBegin, wordEnd);
                    int unusedInt;
                    bool hasSign;
                    if (!ClTokenizer::parseInteger(lineBuffer + wordBegin, wordEnd - wordBegin, fileData.vertexCount, hasSign, unusedInt))
                    {
                        fileData.errorStr = "Could not parse vertex count off ply file.";
                        return offset;
                    }
                }
            }
        }

        if (lineBuffer + lineSize < endBuffer && lineBuffer[lineSize] == '\n') ++lineSize;
        offset += lineSize;
        ++lineIndex;
        if (lineIndex > maxLines)
        {
            fileData.errorStr = "Exceeded header number of lines";
            break;
        }
    }

    if (readState != EndHeader)
    {
        fileData.errorStr = "Did not find end_header token";
    }

    fileData.hasHeader = true;
    return offset;
};

}

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
            size_t readOffset = 0;
            if (!state.plyFileData->hasHeader)
                readOffset = parsePlyHeader(*state.plyFileData, response.buffer, response.size);

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

            loadStatus = SceneLoadStatus::Reading;
        }
        else if (response.status == FileStatus::Success)
        {
            if (state.plyFileData->errorStr != nullptr)
            {
                state.errorStr = state.plyFileData->errorStr;
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
