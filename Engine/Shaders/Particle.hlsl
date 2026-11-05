cbuffer ParticleConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraPosition;
};

struct Instance
{
    float4 positionRotation : PARTICLEPOSITION;
    float4 sizeShape : PARTICLESIZE;
    float4 color : PARTICLECOLOR;
    float4 axisFacing : PARTICLEAXIS;
};

struct Pixel
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation float2 shape : TEXCOORD1;
    float4 color : COLOR0;
};

Pixel VSMain(Instance input, uint vertexId : SV_VertexID)
{
    static const float2 corners[6] = {
        float2(-1, -1), float2(-1, 1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(1, -1)};
    const float2 uv = corners[vertexId];
    float3 axisX = cameraRight.xyz;
    float3 axisY = cameraUp.xyz;
    if (input.axisFacing.w > 0.5 && input.axisFacing.w < 1.5)
    {
        axisY = normalize(float3(input.axisFacing.x, 0, input.axisFacing.z));
        axisX = cross(float3(0, 1, 0), axisY);
    }
    else if (input.axisFacing.w > 1.5)
    {
        axisY = normalize(input.axisFacing.xyz);
        const float3 eye = cameraPosition.xyz - input.positionRotation.xyz;
        float3 across = cross(axisY, eye);
        if (dot(across, across) < 1e-8)
        {
            across = cameraRight.xyz - axisY * dot(cameraRight.xyz, axisY);
        }
        if (dot(across, across) < 1e-8)
        {
            const float3 fallback = abs(axisY.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
            across = cross(axisY, fallback);
        }
        axisX = normalize(across);
    }
    float sine, cosine;
    sincos(input.positionRotation.w, sine, cosine);
    const float2 p = uv * input.sizeShape.xy;
    const float2 rotated = float2(p.x * cosine - p.y * sine, p.x * sine + p.y * cosine);
    const float3 world = input.positionRotation.xyz + axisX * rotated.x + axisY * rotated.y;
    Pixel output;
    output.position = mul(float4(world, 1), viewProjection);
    output.uv = uv;
    output.shape = input.sizeShape.zw;
    output.color = input.color;
    return output;
}

float4 PSMain(Pixel input) : SV_TARGET
{
    const float2 p = abs(input.uv);
    const float polygon = max(p.x, p.y) + 0.41421356 * min(p.x, p.y);
    float coverage = 0;
    if (input.shape.x < 0.5)
    {
        coverage = 1 - smoothstep(0.80, 1.0, p.x + p.y);
        input.color.rgb *= input.uv.x + input.uv.y > 0 ? 1.0 : 0.68;
    }
    else if (input.shape.x < 1.5)
    {
        coverage = pow(saturate(1 - dot(input.uv, input.uv)), 2);
    }
    else if (input.shape.x < 2.5)
    {
        const float r = length(input.uv);
        coverage = smoothstep(0.76, 0.82, r) * (1 - smoothstep(0.95, 1.0, r)) *
            smoothstep(input.shape.y, input.shape.y + 0.08, input.uv.y / max(r, 0.001));
    }
    else if (input.shape.x < 3.5)
    {
        coverage = 1 - smoothstep(0.3, 1.0, polygon);
    }
    else if (input.shape.x < 4.5)
    {
        coverage = (1 - smoothstep(0.12, 0.9, p.x)) * (1 - smoothstep(0.6, 1.0, p.y));
    }
    else
    {
        coverage = smoothstep(0.78, 0.88, polygon) * (1 - smoothstep(0.92, 1.0, polygon));
    }
    const float alpha = input.color.a * coverage;
    clip(alpha - 0.003);
    return float4(input.color.rgb * alpha, alpha);
}
