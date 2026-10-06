#include <metal_stdlib>
using namespace metal;

struct PresentationVertex {
    float4 position [[position]];
    float2 uv;
};

vertex PresentationVertex presentationVertex(uint index [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const float2 position = positions[index];
    return {float4(position, 0, 1), float2((position.x + 1) * 0.5f, (1 - position.y) * 0.5f)};
}

fragment float4 presentationFragment(PresentationVertex input [[stage_in]], texture2d<float> source [[texture(0)]], sampler filtering [[sampler(0)]]) {
    return source.sample(filtering, input.uv);
}
