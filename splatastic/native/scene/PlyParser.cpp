#include "PlyParser.h"
#include <utils/ClTokenizer.h>
#include <stdio.h>
#include <string.h>

namespace splatastic
{

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

size_t parsePlyChunk(PlyFileData& fileData, const char* buffer, size_t bufferSize)
{
    if (fileData.errorStr != nullptr)
        return {};

    size_t readOffset = 0ull;
    if (!fileData.hasHeader)
        readOffset = parsePlyHeader(fileData, buffer, bufferSize);

    if (fileData.hasHeader)
    {
        if (fileData.payload == nullptr)
        {
            fileData.payloadSize = fileData.vertexCount * fileData.strideSize;
            fileData.payload = new char[fileData.payloadSize];
            fileData.payloadReadSize = 0;
        }
        
        size_t chunkSize = bufferSize - readOffset;
        size_t leftToRead = fileData.payloadSize - fileData.payloadReadSize;
        chunkSize =  leftToRead < chunkSize ? leftToRead : chunkSize;
        memcpy(
            fileData.payload + fileData.payloadReadSize,
            buffer + readOffset,
            chunkSize);
        fileData.payloadReadSize += chunkSize;
        readOffset += chunkSize;
    }

    return readOffset;
}

}
