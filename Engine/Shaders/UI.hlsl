struct VertexInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

Texture2D<float> glyphAtlas : register(t0);
SamplerState glyphSampler : register(s0);

float4 PSMain(PixelInput input) : SV_TARGET
{
    return float4(input.color.rgb, input.color.a * glyphAtlas.Sample(glyphSampler, input.uv));
}
