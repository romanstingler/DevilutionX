#version 450

layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(set = 0, binding = 1) uniform sampler2D u_palette;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 f_color;

void main()
{
    // Source texture is R8_UNORM containing palette indices in [0, 255].
    uint index = uint(texture(u_source, v_uv).r * 255.0 + 0.5);
    // Palette texture is a 256x1 RGBA8 lookup; x = index / 256, y = 0.5.
    vec2 palUv = vec2((float(index) + 0.5) / 256.0, 0.5);
    f_color = texture(u_palette, palUv);
}