#version 440
// Бахрома на полосе (aa-fringe): вершина выносится на fringePx экранных px за
// кромку, varying vD - знаковое расстояние поперёк штриха в px.
// В QSGMaterialShader то же самое: mvp = qt_Matrix, pxPerUnit - из
// combinedMatrix(); шейдер собирать с BATCHABLE (location 7 не занимать).

layout(location = 0) in vec2 center;
layout(location = 1) in vec2 dir;
layout(location = 2) in float hw;
layout(location = 3) in float s;

layout(location = 0) out float vD;
layout(location = 1) out float vHw;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    float pxPerUnit;
    float minHwPx;
    float fringePx;
};

void main()
{
    float hwPx = max(hw * pxPerUnit, minHwPx);
    float ext = hwPx + fringePx;
    vD = s * ext;
    vHw = hwPx;
    gl_Position = mvp * vec4(center + dir * (ext / pxPerUnit), 0.0, 1.0);
}
