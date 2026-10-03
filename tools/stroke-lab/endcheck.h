#pragma once

#include <vector>

#include "lab.h"

namespace lab {
// Синтетика касания и отрыва для сравнения концов штриха; печатает таблицы.
int runEndCheck(const std::vector<const VariantInfo *> &variants, double width);
} // namespace lab
