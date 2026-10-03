// app-legacy - конвейер приложения с законом толщины Legacy W·(0.25 + 0.75p).
// Числа D2 (ink-beauty-pass, design.md) выбраны на этом законе, и гейт шага 1
// «отслеживание толщины ≤ 0.03» меряется здесь, при --min-width-factor 0.25
// (по умолчанию). Траектория - та же, что у baseline, до бита.

#include "app_pipeline.h"

namespace {

struct AppLegacy : lab::AppPipeline
{
    AppLegacy()
        : AppPipeline(StrokeStyle::Legacy)
    {
    }
};

} // namespace

STROKE_LAB_VARIANT(AppLegacy, "app-legacy",
                   "Конвейер приложения с законом Legacy 0.25+0.75p: давление D2 на прежнем "
                   "законе, сравнивать при --min-width-factor 0.25");
