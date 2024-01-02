#define COARSE_TILE_SIZE 32

cbuffer Constants : register(b0)
{
    uint2 g_viewSize;
    float2 g_viewSizeInv;

    uint2 g_coarseTileViewDims;
    float2 g_coarseTileViewDimsInv;

    float4x4 g_view;
    float4x4 g_proj;
};

//Utility functions
float4 worldToClip(float3 worldPos)
{
    return mul(mul(float4(worldPos,1), g_view), g_proj);
}

float2 clipToUv(float4 clipPos)
{
    float2 ndcPos = clipPos.xy / clipPos.w;
    float2 uv = ndcPos * float2(0.5, -0.5) + 0.5;
    return uv;
}

RWBuffer<uint> g_outCoarseTileCounts : register(u0);

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
    scene.vertexCount = g_splatMetadataBuffer[0];
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
    return asfloat(scene.payload.Load3(index * scene.stride + SPLAT_POS_OFFSET));
}

float loadSplatAlpha(SplatScene scene, int index)
{
    return asfloat(scene.payload.Load(index * scene.stride + SPLAT_ALPHA_OFFSET));
}

float3 loadSplatScale(SplatScene scene, int index)
{
    return asfloat(scene.payload.Load3(index * scene.stride + SPLAT_SCALE_OFFSET));
}

float4 loadSplatRotation(SplatScene scene, int index)
{
    return asfloat(scene.payload.Load4(index * scene.stride + SPLAT_ROT_OFFSET));
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
    #if 0 // does not quite give the correct results?

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

[numthreads(1, 1, 1)]
void csCoarseTileBin(int3 dti : SV_DispatchThreadID)
{
    SplatScene splatScene = loadSplatScene();

    float3 worldPos = loadSplatPosition(splatScene, 0);
    float4 clipPos = worldToClip(worldPos);
    if (any(abs(clipPos.xyz) > clipPos.www))
        return;

    float2 uvPos = clipToUv(clipPos);
    
    uint2 tileCoord = (uint2)floor(uvPos.xy * (float2)g_viewSize / float(COARSE_TILE_SIZE));

    g_outCoarseTileCounts[tileCoord.x + tileCoord.y * g_coarseTileViewDims.x] = 1;
}

RWTexture2D<float4> g_colorBuffer : register(u0);

[numthreads(8,8,1)]
void csRasterSplats(int3 dti : SV_DispatchThreadID)
{
    SplatScene splatScene = loadSplatScene();

    if (any(dti.xy > g_viewSize.xy))
        return;

    float2 screenUv = (dti.xy + 0.5) * float2(g_viewSizeInv.xy);

    float3 col = float3(0,0,0);
    for (int i = 0; i < 256; ++i)
    {
        float3 splatPos = loadSplatPosition(splatScene, i);
        float3 splatScale = loadSplatScale(splatScene, i);
        float4 splatRotation = loadSplatRotation(splatScene, i);
        float3x3 splatTransform = calcMatrixFromRotationScale(splatRotation, splatScale);

        float3 cov3d0, cov3d1;
        calcCovariance3D(splatTransform, cov3d0, cov3d1);

        float4 splatClipPos = worldToClip(splatPos);
        float2 splatScreenUv = clipToUv(splatClipPos);
        float3 cov2d = calcCovariance2D(splatPos, cov3d0, cov3d1, transpose(g_view), transpose(g_proj), (float)g_viewSize.x);

        float2 axis0, axis1;
        decomposeCovariance(cov2d, axis0, axis1);

        float axis0Len = length(axis0);
        float axis1Len = length(axis1);

        axis0 *= rcp(axis0Len);
        axis1 *= rcp(axis1Len);

        float2 splatRelUv = (splatScreenUv - screenUv) * (float2)g_viewSize.xy * 2.0;
        float2 localCoord = float2(dot(axis0, splatRelUv), dot(axis1, splatRelUv));

        float3 debugCol = all(abs(localCoord) < float2(axis0Len, axis1Len)) ? float3(1,0,0) : float3(0,0,0);
        if (splatClipPos.z > splatClipPos.w)
            debugCol = float3(0,0,0);

        col += debugCol * 0.1;
    }

    g_colorBuffer[dti.xy] = float4(col, 1.0);
}
