#version 440
// Точное покрытие пиксельным боксом поперёк штриха (variants/aa_fringe.cpp).

layout(location = 0) in float vD;
layout(location = 1) in float vHw;
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
    float ad = abs(vD);
    float cov = clamp(min(ad + vHw, 0.5) - max(ad - vHw, -0.5), 0.0, 1.0);
    fragColor = color * cov; // premultiplied
}
