#define ENGINE_SKINNED_VERTEX_SHADER
#include "StaticMesh.hlsl"

cbuffer SkinConstants : register(b1)
{
    row_major float4x4 palettes[128];
};

struct SkinnedVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

float3 SkinnedNormal(float3 normal, float3x3 skinLinear)
{
    const float largest = max(max(max(abs(skinLinear[0].x), abs(skinLinear[0].y)), abs(skinLinear[0].z)),
        max(max(max(abs(skinLinear[1].x), abs(skinLinear[1].y)), abs(skinLinear[1].z)),
            max(max(abs(skinLinear[2].x), abs(skinLinear[2].y)), abs(skinLinear[2].z))));
    float3 result = float3(0.0f, 0.0f, 0.0f);
    if (largest > 0.0f && isfinite(largest))
    {
        const float3 a = skinLinear[0] / largest;
        const float3 b = skinLinear[1] / largest;
        const float3 c = skinLinear[2] / largest;
        const float3 first = cross(b, c);
        const float determinant = dot(a, first);
        if (determinant != 0.0f && isfinite(determinant))
        {
            // Cofactor/determinant is inverse-transpose. Its positive magnitude cancels on normalization.
            const float3x3 cofactor = float3x3(first, cross(c, a), cross(a, b));
            result = NormalizeDirection(mul(normal, cofactor) * (determinant < 0.0f ? -1.0f : 1.0f));
        }
    }
    // Linear blend skinning can collapse even when every bone matrix is nonsingular.
    return result;
}

PixelInput VSMain(SkinnedVertexInput input)
{
    const float4x4 skin = palettes[input.joints.x] * input.weights.x +
        palettes[input.joints.y] * input.weights.y +
        palettes[input.joints.z] * input.weights.z +
        palettes[input.joints.w] * input.weights.w;
    const float3 skinnedPosition = mul(float4(input.position, 1.0f), skin).xyz;
    const float3 skinnedNormal = SkinnedNormal(input.normal, (float3x3)skin);
    const float4 worldPosition = mul(float4(skinnedPosition, 1.0f), world);
    PixelInput output;
    output.position = mul(worldPosition, viewProjection);
    output.worldNormal = mul(skinnedNormal, (float3x3)normalWorld);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.tint = float4(1, 1, 1, 1);
    output.effects = objectParams.xyz;
    return output;
}
