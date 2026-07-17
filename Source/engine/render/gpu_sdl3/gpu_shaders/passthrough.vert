#version 450

layout(location = 0) out vec2 v_uv;

void main()
{
    // Fullscreen triangle covering NDC [-1, 3] in x and y.
    vec2 pos = vec2(((gl_VertexIndex << 1) & 2) * 2.0 - 1.0,
                    (gl_VertexIndex & 2) * 2.0 - 1.0);
    v_uv = pos * 0.5 + 0.5;
    // Flip Y so the SDL surface's top-left origin matches the window's top-left.
    v_uv.y = 1.0 - v_uv.y;
    gl_Position = vec4(pos, 0.0, 1.0);
}