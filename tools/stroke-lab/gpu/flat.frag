#version 440

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    float pxPerUnit;
    float minHwPx;
    float fringePx;
};

void main()
{
    fragColor = color;
}
