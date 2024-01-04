#define COARSE_TILE_SIZE 32

#define USE_TEST_DATA 0

cbuffer Constants : register(b0)
{
    uint2 g_viewSize;
    float2 g_viewSizeInv;

    uint2 g_coarseTileViewDims;
    float2 g_coarseTileViewDimsInv;

    uint g_coarseTileRecordMax;
    uint3 g_unused0;

    float4x4 g_view;
    float4x4 g_proj;
};

//Utility functions
float4 worldToClip(float3 worldPos)
{
    return mul(g_proj, mul(g_view, float4(worldPos, 1.0)));
}

float3 worldToView(float3 worldPos)
{
    return mul(g_view, float4(worldPos, 1.0)).xyz;
}

float4 viewToClip(float3 viewPos)
{
    return mul(g_proj, float4(viewPos, 1.0));
}

float2 ndcToUv(float2 ndc)
{
    return ndc * float2(0.5, -0.5) + 0.5;
}

float2 clipToUv(float4 clipPos)
{
    float2 ndcPos = clipPos.xy / clipPos.w;
    float2 uv = ndcToUv(ndcPos);
    return uv;
}

Buffer<uint> g_splatMetadataBuffer : register(t0);
ByteAddressBuffer g_splatPayloadBuffer : register(t1);

struct SplatScene
{
    int vertexCount;
    int stride;
    ByteAddressBuffer payload;
};

SplatScene loadSplatScene()
{
    SplatScene scene;
#if USE_TEST_DATA
    scene.vertexCount = 3;
#else
    scene.vertexCount = g_splatMetadataBuffer[0];
#endif
    scene.stride = g_splatMetadataBuffer[1];
    scene.payload = g_splatPayloadBuffer;
    return scene;
}

#define SPLAT_POS_OFFSET 0
#define SPLAT_ALPHA_OFFSET (54 << 2)
#define SPLAT_SCALE_OFFSET (55 << 2)
#define SPLAT_ROT_OFFSET (58 << 2)

float3 loadSplatPosition(SplatScene scene, int index)
{
#if USE_TEST_DATA
    return float3(4 * index, 0.0, 0.0);
#else
    return asfloat(scene.payload.Load3(index * scene.stride + SPLAT_POS_OFFSET));
#endif
}

float loadSplatAlpha(SplatScene scene, int index)
{
#if USE_TEST_DATA
    return 1.0;
#else
    return asfloat(scene.payload.Load(index * scene.stride + SPLAT_ALPHA_OFFSET));
#endif
}

float3 loadSplatScale(SplatScene scene, int index)
{
#if USE_TEST_DATA
    if (index == 0)
        return float3(2,1,1);
    else
        return float3(1,1,1);
#else
    return asfloat(scene.payload.Load3(index * scene.stride + SPLAT_SCALE_OFFSET));
#endif
}

float4 loadSplatRotation(SplatScene scene, int index)
{
#if USE_TEST_DATA
    return float4(0,0,0,0);
#else
    return asfloat(scene.payload.Load4(index * scene.stride + SPLAT_ROT_OFFSET));
#endif
}

//// taken from UnityGaussianSplatting ////
// Aras P., https://github.com/aras-p/UnityGaussianSplatting

float3x3 calcMatrixFromRotationScale(float4 rot, float3 scale)
{
    float3x3 ms = float3x3(
        scale.x, 0, 0,
        0, scale.y, 0,
        0, 0, scale.z
    );
    float x = rot.x;
    float y = rot.y;
    float z = rot.z;
    float w = rot.w;
    float3x3 mr = float3x3(
        1-2*(y*y + z*z),   2*(x*y - w*z),   2*(x*z + w*y),
          2*(x*y + w*z), 1-2*(x*x + z*z),   2*(y*z - w*x),
          2*(x*z - w*y),   2*(y*z + w*x), 1-2*(x*x + y*y)
    );
    return mul(mr, ms);
}

void calcCovariance3D(float3x3 rotMat, out float3 sigma0, out float3 sigma1)
{
    float3x3 sig = mul(rotMat, transpose(rotMat));
    sigma0 = float3(sig._m00, sig._m01, sig._m02);
    sigma1 = float3(sig._m11, sig._m12, sig._m22);
}

// from "EWA Splatting" (Zwicker et al 2002) eq. 31
float3 calcCovariance2D(float3 worldPos, float3 cov3d0, float3 cov3d1, float4x4 matrixV, float4x4 matrixP, float screenWidth)
{
    float4x4 viewMatrix = matrixV;
    float3 viewPos = mul(viewMatrix, float4(worldPos, 1)).xyz;

    // this is needed in order for splats that are visible in view but clipped "quite a lot" to work
    float aspect = matrixP._m00 / matrixP._m11;
    float tanFovX = rcp(matrixP._m00);
    float tanFovY = rcp(matrixP._m11 * aspect);
    float limX = 1.3 * tanFovX;
    float limY = 1.3 * tanFovY;
    viewPos.x = clamp(viewPos.x / viewPos.z, -limX, limX) * viewPos.z;
    viewPos.y = clamp(viewPos.y / viewPos.z, -limY, limY) * viewPos.z;

    float focal = screenWidth * matrixP._m00 / 2;

    float3x3 J = float3x3(
        focal / viewPos.z, 0, -(focal * viewPos.x) / (viewPos.z * viewPos.z),
        0, focal / viewPos.z, -(focal * viewPos.y) / (viewPos.z * viewPos.z),
        0, 0, 0
    );
    float3x3 W = (float3x3)viewMatrix;
    float3x3 T = mul(J, W);
    float3x3 V = float3x3(
        cov3d0.x, cov3d0.y, cov3d0.z,
        cov3d0.y, cov3d1.x, cov3d1.y,
        cov3d0.z, cov3d1.y, cov3d1.z
    );
    float3x3 cov = mul(T, mul(V, transpose(T)));

    // Low pass filter to make each splat at least 1px size.
    cov._m00 += 0.3;
    cov._m11 += 0.3;
    return float3(cov._m00, cov._m01, cov._m11);

}

void decomposeCovariance(float3 cov2d, out float2 v1, out float2 v2)
{
    #if 0// does not quite give the correct results?

    // https://jsfiddle.net/mattrossman/ehxmtgw6/
    // References:
    // - https://www.youtube.com/watch?v=e50Bj7jn9IQ
    // - https://en.wikipedia.org/wiki/Eigenvalue_algorithm#2%C3%972_matrices
    // - https://people.math.harvard.edu/~knill/teaching/math21b2004/exhibits/2dmatrices/index.html
    float a = cov2d.x;
    float b = cov2d.y;
    float d = cov2d.z;
    float det = a * d - b * b; // matrix is symmetric, so "c" is same as "b"
    float trace = a + d;

    float mean = 0.5 * trace;
    float dist = sqrt(mean * mean - det);

    float lambda1 = mean + dist; // 1st eigenvalue
    float lambda2 = mean - dist; // 2nd eigenvalue

    if (b == 0) {
        // https://twitter.com/the_ross_man/status/1706342719776551360
        if (a > d) v1 = float2(1, 0);
        else v1 = float2(0, 1);
    } else
        v1 = normalize(float2(b, d - lambda2));

    v1.y = -v1.y;
    // The 2nd eigenvector is just a 90 degree rotation of the first since Gaussian axes are orthogonal
    v2 = float2(v1.y, -v1.x);

    // scaling components
    v1 *= sqrt(lambda1);
    v2 *= sqrt(lambda2);

    float radius = 1.5;
    v1 *= radius;
    v2 *= radius;

    #else

    // same as in antimatter15/splat
    float diag1 = cov2d.x, diag2 = cov2d.z, offDiag = cov2d.y;
    float mid = 0.5f * (diag1 + diag2);
    float radius = length(float2((diag1 - diag2) / 2.0, offDiag));
    float lambda1 = mid + radius;
    float lambda2 = max(mid - radius, 0.1);
    float2 diagVec = normalize(float2(offDiag, lambda1 - diag1));
    diagVec.y = -diagVec.y;
    float maxSize = 4096.0;
    v1 = min(sqrt(2.0 * lambda1), maxSize) * diagVec;
    v2 = min(sqrt(2.0 * lambda2), maxSize) * float2(diagVec.y, -diagVec.x);

    #endif
}

/////

uint2 packCoarseTile(uint tileAddress, uint tileOffset, uint splatID)
{
    return uint2((tileAddress & 0xFFFF) | (tileOffset << 16), splatID);
}

void unpackCoarseTile(uint2 coarseTile, out uint tileAddress, out uint tileOffset, out uint splatID)
{
    tileAddress = coarseTile.x & 0xFFFF;
    tileOffset = coarseTile.x >> 16;
    splatID = coarseTile.y;
}

RWBuffer<uint> g_outCoarseTileCounts : register(u0);
RWBuffer<uint> g_outCoarseTileRecordCounter : register(u1);
RWBuffer<uint2> g_outCoarseTileRecordBuffer : register(u2);

#define COARSE_TILE_BIN_THREADS 128
[numthreads(COARSE_TILE_BIN_THREADS, 1, 1)]
void csCoarseTileBin(uint3 dti : SV_DispatchThreadID, uint gti : SV_GroupThreadID)
{
    SplatScene splatScene = loadSplatScene();
    uint threadID = dti.x;

    if (threadID >= splatScene.vertexCount)
        return;

    uint splatID = threadID;
    float3 worldPos = loadSplatPosition(splatScene, splatID);
    float3 viewPos = worldToView(worldPos);
    float4 clipPos = viewToClip(viewPos);
    if (any(abs(clipPos.z) >= clipPos.w) || any(abs(clipPos.xy) >= clipPos.w * 2.0))
        return;

    float3 splatScale = loadSplatScale(splatScene, splatID);
    float rad = length(splatScale);
    float4 clipEnd = viewToClip(viewPos + rad);

    float2 uvCenter = ndcToUv(clipPos.xy / clipPos.w);
    float2 uvCorner = ndcToUv(clipEnd.xy / clipEnd.w);
    float2 uvDiff = abs(uvCorner - uvCenter) * 1.9;
    float2 aabbBegin = uvCenter - uvDiff;
    float2 aabbEnd = uvCenter + uvDiff;

    if (any(uvDiff < 16.0 * g_viewSizeInv.xy) || any(uvDiff > 4.0) || any(aabbBegin >= float2(1.0,1.0)) || any(aabbEnd <= float2(0.0,0.0)))
        return;

    aabbBegin = saturate(aabbBegin);
    aabbEnd = saturate(aabbEnd);
    
    int2 tileBegin = (int2)floor(aabbBegin.xy * (float2)g_viewSize / float(COARSE_TILE_SIZE));
    int2 tileEnd = (int2)floor(aabbEnd.xy * (float2)g_viewSize / float(COARSE_TILE_SIZE));

    for (int i = tileBegin.x; i <= tileEnd.x; ++i)
    {
        for (int j = tileBegin.y; j <= tileEnd.y; ++j)
        {
            uint2 tileCoord = int2(i, j);
            uint tileAddress = tileCoord.x + tileCoord.y * g_coarseTileViewDims.x;
            uint coarseTileOffset = 0;
            uint globalOffset = 0;
            InterlockedAdd(g_outCoarseTileCounts[tileAddress], 1, coarseTileOffset);
            InterlockedAdd(g_outCoarseTileRecordCounter[0], 1, globalOffset);

            uint2 packedTile = packCoarseTile(tileAddress, coarseTileOffset, splatID);
            g_outCoarseTileRecordBuffer[globalOffset] = packedTile;
        }
    }
}


Buffer<uint> g_createArgsCounterBuffer : register(t0);
RWBuffer<uint4> g_outArgsBuffer : register(u0);

[numthreads(1,1,1)]
void csCreateCoarseTileDispatchArgs(int3 dti : SV_DispatchThreadID)
{
    g_outArgsBuffer[0] = uint4(((g_createArgsCounterBuffer[0] + 63) / 64), 1, 1, 0);
}

Buffer<uint> g_createListRecordCountBuffer : register(t0);
Buffer<uint> g_createListOffsets : register(t1);
Buffer<uint2> g_createListRecords : register(t2);
RWBuffer<uint> g_outCreateListData : register(u0);

[numthreads(64,1,1)]
void csCreateCoarseTileList(uint3 dti : SV_DispatchThreadID)
{
    uint threadID = dti.x;
    uint recordCount = g_createListRecordCountBuffer[0];
    if (threadID >= recordCount)
        return;

    uint2 coarseListRecord = g_createListRecords[threadID];
    uint tileAddress, tileOffset, splatID;
    unpackCoarseTile(coarseListRecord, tileAddress, tileOffset, splatID);

    uint outputOffset = g_createListOffsets[tileAddress] + tileOffset;
    g_outCreateListData[outputOffset] = splatID;
}

//Buffer<uint> g_splatMetadataBuffer : register(t0);
//ByteAddressBuffer g_splatPayloadBuffer : register(t1);
Buffer<uint> g_coarseTileOffsets : register(t2);
Buffer<uint> g_coarseTileCounts : register(t3);
Buffer<uint> g_coarseTileData : register(t4);
RWTexture2D<float4> g_colorBuffer : register(u0);

[numthreads(8,8,1)]
void csRasterSplats(int3 dti : SV_DispatchThreadID)
{
    SplatScene splatScene = loadSplatScene();

    if (any(dti.xy > g_viewSize.xy))
        return;

    uint2 tileID = dti.xy / COARSE_TILE_SIZE;
    uint tileAddress = tileID.x + tileID.y * g_coarseTileViewDims.x;
    uint tileOffset = g_coarseTileOffsets[tileAddress];
    uint tileCount = g_coarseTileCounts[tileAddress];

    float2 screenUv = (dti.xy + 0.5) * float2(g_viewSizeInv.xy);

    float4 col = float4(0,0,0,0);
    tileCount = min(tileCount, 250);
    float weights = 0.0;
    for (int i = 0; i < tileCount; ++i)
    {
        uint splatID = g_coarseTileData[tileOffset + i];
        float3 splatPos = loadSplatPosition(splatScene, splatID);
        float3 splatScale = loadSplatScale(splatScene, splatID);
        float4 splatRotation = loadSplatRotation(splatScene, splatID);
    
        float3x3 splatTransform = calcMatrixFromRotationScale(splatRotation, splatScale);

        float3 cov3d0, cov3d1;
        calcCovariance3D(splatTransform, cov3d0, cov3d1);

        float4 splatClipPos = worldToClip(splatPos);
        float2 splatScreenUv = clipToUv(splatClipPos);
        float3 cov2d = calcCovariance2D(splatPos, cov3d0, cov3d1, g_view, g_proj, (float)g_viewSize.x);

        float2 axis0, axis1;
        decomposeCovariance(cov2d, axis0, axis1);

#if 1
        float lenAxis0 = dot(axis0, axis0);
        float lenAxis1 = dot(axis1, axis1);

        float2 splatRelUv = (splatScreenUv - screenUv) * (float2)g_viewSize;
        splatRelUv = float2(dot(axis0, splatRelUv), dot(axis1, splatRelUv))/float2(lenAxis0, lenAxis1);

        float2 localCoord = splatRelUv;
#endif
        
        //float4 debugCol = float4(length(localCoord).xxx, 1.0);
        float4 debugCol = float4(exp(-dot(localCoord, localCoord)).xxx, 1.0);
        debugCol.w *= (abs(splatClipPos.z) > splatClipPos.w) ? 0.0 : 1.0;
        col += debugCol;
    }

    col = col.w == 0 ? float4(0,0,0,0) : col;// / col.w;
    if (tileOffset >= g_coarseTileRecordMax)
        col.rgb = float3(1,0,0);
    g_colorBuffer[dti.xy] = col * 0.5;
}
