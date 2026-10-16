cbuffer DrawConstants : register(b0)
{
    row_major float4x4 world;
    row_major float4x4 normalWorld;
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirectionAmbient;
    float4 lightColorIntensity;
    float4 materialColor;
    float4 blendColor;
    float4 blendParams;
};

Texture2D diffuseTexture : register(t0);
Texture2D blendTexture : register(t1);
SamplerState diffuseSampler : register(s0);

cbuffer EffectsConstants : register(b2)
{
    row_major float4x4 lightViewProjection;
    float4 shadowParams; // enabled, receiver depth bias, world normal bias, texel size
    float4 objectParams; // coverage, alpha cutoff, receives shadow, PCF radius
    float4 specularParams; // intensity, power, unused, unused
};

Texture2D<float> shadowTexture : register(t2);
SamplerComparisonState shadowSampler : register(s1);

#ifndef ENGINE_SKINNED_VERTEX_SHADER
struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
#endif

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 worldPosition : TEXCOORD2;
    nointerpolation float4 tint : TEXCOORD3;
    nointerpolation float3 effects : TEXCOORD4;
};

#ifndef ENGINE_SKINNED_VERTEX_SHADER
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    const float4 worldPosition = mul(float4(input.position, 1.0f), world);
    output.position = mul(worldPosition, viewProjection);
    output.worldNormal = mul(input.normal, (float3x3)normalWorld);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.tint = float4(1, 1, 1, 1);
    output.effects = objectParams.xyz;
    return output;
}
#endif

float3 LinearToSrgb(float3 linearColor)
{
    linearColor = max(linearColor, 0.0f);
    const float3 low = 12.92f * linearColor;
    const float3 high = 1.055f * pow(linearColor, 1.0f / 2.4f) - 0.055f;
    return float3(
        linearColor.r <= 0.0031308f ? low.r : high.r,
        linearColor.g <= 0.0031308f ? low.g : high.g,
        linearColor.b <= 0.0031308f ? low.b : high.b);
}

float3 NormalizeDirection(float3 direction)
{
    const float largest = max(max(abs(direction.x), abs(direction.y)), abs(direction.z));
    // Opposing interpolated normals can cancel; use ambient light for that pixel.
    float3 result = float3(0.0f, 0.0f, 0.0f);
    if (largest > 0.0f)
    {
        const float3 scaled = direction / largest;
        result = scaled * rsqrt(dot(scaled, scaled));
    }
    return result;
}

float ShadowVisibility(float3 position, float3 normal)
{
    float visibility = 1.0f;
    const float4 lightPosition = mul(float4(position + normal * shadowParams.z, 1.0f), lightViewProjection);
    if (shadowParams.x > 0.5f && lightPosition.w > 0.0f)
    {
        const float3 projected = lightPosition.xyz / lightPosition.w;
        const float2 uv = float2(projected.x * 0.5f + 0.5f, 0.5f - projected.y * 0.5f);
        if (all(uv >= 0.0f) && all(uv <= 1.0f) && projected.z >= 0.0f && projected.z <= 1.0f)
        {
            const int radius = (int)objectParams.w;
            float sum = 0.0f;
            for (int y = -radius; y <= radius; ++y)
            {
                for (int x = -radius; x <= radius; ++x)
                {
                    sum += shadowTexture.SampleCmpLevelZero(shadowSampler,
                        uv + float2(x, y) * shadowParams.w, projected.z - shadowParams.y);
                }
            }
            const float width = (float)(radius * 2 + 1);
            visibility = sum / (width * width);
        }
    }
    return visibility;
}

float4 SampleAlbedo(PixelInput input)
{
    // An sRGB SRV decodes texture RGB to linear; alpha remains linear.
    float4 albedo = diffuseTexture.Sample(diffuseSampler, input.uv) * materialColor * input.tint;
    if (blendParams.z > 0.5f)
    {
        const float height = clamp(input.worldPosition.y, blendParams.x, blendParams.y);
        const float weight = saturate((height - blendParams.x) / (blendParams.y - blendParams.x));
        const float4 secondAlbedo = blendTexture.Sample(diffuseSampler, input.uv) * blendColor * input.tint;
        albedo = lerp(albedo, secondAlbedo, weight);
    }
    return albedo;
}

void PSShadow(PixelInput input)
{
    // Camera coverage deliberately does not remove shadow casters.
    if (input.effects.y > 0.0f)
    {
        clip(SampleAlbedo(input).a - input.effects.y);
    }
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    const float4 albedo = SampleAlbedo(input);
    if (input.effects.y > 0.0f)
    {
        clip(albedo.a - input.effects.y);
    }
    static const uint ranks[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const uint2 pixel = uint2(input.position.xy) & 3;
    const float threshold = ((float)ranks[pixel.y * 4 + pixel.x] + 0.5f) / 16.0f;
    clip(input.effects.x - threshold);
    const float3 normal = NormalizeDirection(input.worldNormal);
    // direction is the travel direction of light, so surfaces face its negative.
    const float diffuse = saturate(dot(normal, -lightDirectionAmbient.xyz));
    float visibility = 1.0f;
    if (shadowParams.x > 0.5f && input.effects.z > 0.5f)
    {
        visibility = ShadowVisibility(input.worldPosition, normal);
    }
    const float3 lighting = lightDirectionAmbient.w +
        lightColorIntensity.rgb * lightColorIntensity.w * diffuse * visibility;
    float3 linearColor = albedo.rgb * lighting;
    if (specularParams.x > 0.0f && diffuse > 0.0f)
    {
        const float3 eye = NormalizeDirection(cameraPosition.xyz - input.worldPosition);
        const float3 halfway = NormalizeDirection(eye - lightDirectionAmbient.xyz);
        const float specular = pow(saturate(dot(normal, halfway)), specularParams.y) * specularParams.x;
        linearColor += lightColorIntensity.rgb * lightColorIntensity.w * specular * visibility;
    }
    // The current backbuffer RTV is UNORM, so the shader explicitly encodes display RGB.
    return float4(saturate(LinearToSrgb(linearColor)), saturate(albedo.a));
}
