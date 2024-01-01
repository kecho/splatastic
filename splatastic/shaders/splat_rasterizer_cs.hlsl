
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

[numthreads(1, 1, 1)]
void csCoarseTileBin(int3 dti : SV_DispatchThreadID)
{
    float4 clipPos = worldToClip(float3(0,0,0));
    if (any(abs(clipPos.xyz) > clipPos.www))
        return;

    float2 uvPos = clipToUv(clipPos);
    uint2 tileCoord = (uint2)floor(uvPos.xy * (float2)g_viewSize * g_coarseTileViewDimsInv);

    g_outCoarseTileCounts[tileCoord.x + tileCoord.y * g_coarseTileViewDims.x] = 1;
}
