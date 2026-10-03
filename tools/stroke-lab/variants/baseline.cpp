// baseline - конвейер приложения как есть (app_pipeline.h). Новые штрихи пера
// приложение рисует законом Tapered, поэтому и baseline - тоже: его толщину
// сравнивать с эталоном при --min-width-factor 0.08.

#include "app_pipeline.h"

namespace {

struct Baseline : lab::AppPipeline
{
    Baseline()
        : AppPipeline(StrokeStyle::Tapered)
    {
    }
};

} // namespace

STROKE_LAB_VARIANT(Baseline, "baseline",
                   "Конвейер приложения: StrokeSampler (ink-stroke-modeler + follow + "
                   "Catmull-Rom 1.2 px, давление по времени узла) -> WetStroke -> "
                   "StrokeTessellator, закон Tapered 0.08+0.92p, MSAA 4x");
