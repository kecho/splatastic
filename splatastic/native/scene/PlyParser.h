#pragma once

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

size_t parsePlyChunk(PlyFileData& fileData, const char* buffer, size_t bufferSize);

}
