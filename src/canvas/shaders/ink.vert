#version 440
// Материал чернил (design.md ink-beauty-pass, D7). Вершина AxisVertex
// (src/ink/stroketessellator.h) выносится на fringePx device px за кромку:
// pos = c + n̂·(hwPx + fringePx), varying vD = s·вынос - знаковое расстояние
// поперёк штриха в device px. Вынос одинаков у всех вершин, и у внутренней
// вершины стыка тоже: предела по радиусу кривизны нет (D7). У центра веера
// направление нулевое - вершина остаётся в центре, d = 0. Пересчёт в device
// px - здесь, по pxPerUnit из матрицы и dpr, поэтому зум не пересобирает
// геометрию. CPU-зеркало - src/ink/inkcoverage.h.
// Собирается с BATCHABLE: location 7 занимает батч-рендер Qt Quick.

layout(location = 0) in vec2 center;
layout(location = 1) in vec2 dir;
layout(location = 2) in float hw;
layout(location = 3) in float s;

layout(location = 0) out float vD;
layout(location = 1) out float vHw;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 color;      // premultiplied, с прозрачностью узла
    float pxPerUnit; // device px в точке страницы
    float minHwPx;   // пол полуширины, device px
    float fringePx;  // вынос за кромку, device px
};

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    float k = max(pxPerUnit, 1e-6);
    float hwPx = max(hw * k, minHwPx);
    float ext = hwPx + fringePx;
    float len = length(dir);
    vec2 unitDir = len > 0.0 ? dir / len : vec2(0.0);
    vD = s * ext;
    vHw = hwPx;
    gl_Position = qt_Matrix * vec4(center + unitDir * (ext / k), 0.0, 1.0);
}
