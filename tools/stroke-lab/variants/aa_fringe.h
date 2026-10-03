#pragma once

// Вершины aa-fringe в том виде, в каком они легли бы в VBO для шейдера
// (tools/stroke-lab/gpu/ink.vert): координаты страницы, без выноса бахромы.

#include <vector>

#include "lab.h"

namespace lab {

struct FringeGpuVertex
{
    float cx, cy; // центр стыка, точки страницы
    float nx, ny; // единичное направление выноса (0,0 у центра веера)
    float hw;     // полуширина, точки страницы
    float s;      // d = s * (hwPx + e): ±1, ±cos(θ/2) у биссектрисы, 0 у центра
};

// Полоса aa-fringe варианта после finish(); false, если variant - не aa-fringe*.
bool fringeGpuStrip(const Variant &variant, std::vector<FringeGpuVertex> &out);

} // namespace lab
