#pragma once

// Метрики растеризации: насколько пиксели варианта близки к точному покрытию
// и насколько они устойчивы к субпиксельному сдвигу. Траектория здесь не
// оценивается - только «как та же геометрия легла в пиксели».
//
// Все ошибки - в долях покрытия ×255 (то же, что уровни яркости для чёрных
// чернил на белом). Покрытие из картинки варианта восстанавливается проекцией
// цвета пикселя на отрезок бумага -> чернила.
//
//  aa_rmse_shape / aa_p99_shape / aa_max_shape  - против точного покрытия
//      собственной геометрии варианта (64 точки решётки ранга 1,
//      SamplePattern::lattice64; ошибка эталона ~1/64): чистая ошибка
//      сглаживания. Пиксели - только кромка: частичное покрытие хотя бы в
//      одном из двух изображений.
//  aa_rmse_ideal - против идеальной круглой кисти по осевой варианта (lattice64):
//      сглаживание + форма стыков и торцов.
//  aa_rmse_truth - против эталона (траектория без шума): всё вместе.
//  aa_flicker - «дыхание» кромки при пане: pageToPixel сдвигается на
//      shifts субпиксельных смещений, для каждого пикселя берётся std по
//      сдвигам ошибки (вариант − точное покрытие формы), среднее по пикселям.
//  aa_mass_breath_pct - std/mean суммарной массы чернил по сдвигам, %.
//  aa_mass_ratio - масса чернил варианта / масса идеальной кисти (сдвиг 0).
//  aa_mass_cv - «бусины»: масса чернил в отрезках осевой по 2 px,
//      отношение вариант/идеал, коэффициент вариации по отрезкам.
//  marker_dark_frac - тот же штрих полупрозрачным маркером (#66f2d024):
//      доля пикселей маркера, где итоговая альфа больше 0.4 + 0.01 (двойное
//      наложение на складках и самопересечениях); marker_alpha_max.

#include <QColor>
#include <QImage>
#include <QJsonObject>

#include <vector>

#include "corpus.h"
#include "metrics.h"
#include "render.h"

namespace lab {

struct RasterMetricsOptions
{
    int shifts = 8;
    bool marker = true;
    QColor markerColor = QColor::fromRgba(0x66f2d024);
};

struct RasterMetricsResult
{
    QJsonObject metrics;
    QImage marker; // 1:1, маркер на белом
    // Карта ошибки сдвига 0: серый = 0, красный - вариант темнее точного
    // покрытия формы, синий - светлее; насыщенность ×4.
    QImage errorMap;
};

RasterMetricsResult computeRasterMetrics(const Sample &sample, const std::vector<StrokeRun> &runs,
                                         double scale, const RenderOptions &options,
                                         const RasterMetricsOptions &rasterOptions);

} // namespace lab
