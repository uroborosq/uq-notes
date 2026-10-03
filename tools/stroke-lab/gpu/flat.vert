#version 440
// Как QSGFlatColorMaterial: позиция и сплошной цвет.

layout(location = 0) in vec2 pos;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    float pxPerUnit;
    float minHwPx;
    float fringePx;
};

void main()
{
    gl_Position = mvp * vec4(pos, 0.0, 1.0);
}
