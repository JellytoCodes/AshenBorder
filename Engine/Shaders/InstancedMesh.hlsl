#define ENGINE_SKINNED_VERTEX_SHADER
#include "StaticMesh.hlsl"

struct InstanceVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 world0 : INSTANCEWORLD0;
    float4 world1 : INSTANCEWORLD1;
    float4 world2 : INSTANCEWORLD2;
    float4 world3 : INSTANCEWORLD3;
    float4 normal0 : INSTANCENORMAL0;
    float4 normal1 : INSTANCENORMAL1;
    float4 normal2 : INSTANCENORMAL2;
    float4 normal3 : INSTANCENORMAL3;
    float4 tint : INSTANCETINT;
    float4 effects : INSTANCEEFFECTS;
};

PixelInput VSMain(InstanceVertexInput input)
{
    const float4x4 instanceWorld = float4x4(input.world0, input.world1, input.world2, input.world3);
    const float4x4 instanceNormal = float4x4(input.normal0, input.normal1, input.normal2, input.normal3);
    const float4 position = mul(float4(input.position, 1), instanceWorld);
    PixelInput output;
    output.position = mul(position, viewProjection);
    output.worldNormal = mul(input.normal, (float3x3)instanceNormal);
    output.worldPosition = position.xyz;
    output.uv = input.uv;
    output.tint = input.tint;
    output.effects = input.effects.xyz;
    return output;
}
