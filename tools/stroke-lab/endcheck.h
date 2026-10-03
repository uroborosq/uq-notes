#pragma once

#include <vector>

#include "lab.h"

namespace lab {
// Синтетика касания и отрыва для сравнения концов штриха; печатает таблицы.
// minWidthFactor - нижний предел эталонного закона w·(f + (1−f)·p), как
// --min-width-factor у метрик корпуса.
int runEndCheck(const std::vector<const VariantInfo *> &variants, double width,
                double minWidthFactor);
} // namespace lab
