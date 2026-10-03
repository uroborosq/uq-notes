#pragma once

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QString>

#include <vector>

#include "corpus.h"
#include "metrics.h"

namespace lab {

struct RenderOptions
{
    double dpr = 1.0;                       // физических пикселей в логическом
    QColor paper = QColor(255, 255, 255);   // фон страницы
    QColor ink = QColor(0x1c, 0x2a, 0x5a);  // m_penColor приложения
    MetricsOptions metrics;
};

// Как в приложении: сухая геометрия варианта, MSAA 4x (или своя растеризация
// варианта).
QImage renderApp(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                 const RenderOptions &options);
// Осевая линия варианта, нарисованная «идеальной» круглой кистью (диски +
// трапеции) с покрытием 8x8: что дала бы та же осевая при идеальной
// тесселяции и сглаживании.
QImage renderIdeal(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                   const RenderOptions &options);
// Эталон: траектория без тремора и шума, толщина по эталонному давлению.
QImage renderTruth(const Sample &sample, double scale, const RenderOptions &options);
// Кадр мокрого штриха посреди письма, x4: мокрая геометрия (MSAA 4x) плюс
// отсчёты пера (оранжевые), эталон (зелёный), провизорный хвост (красный),
// положение пера (синий крест), кончик мокрой линии (красное кольцо).
QImage renderWetFrame(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                      const RenderOptions &options);

// Точка интереса для увеличенного фрагмента: первый острый угол или место
// наибольшей кривизны эталона. Экранные px.
QPointF focusPoint(const Sample &sample);
// Фрагмент w x h экранных px вокруг центра, увеличенный nearest-neighbour.
QImage zoomCrop(const QImage &image, const QPointF &centerPx, double dpr, int w, int h,
                int zoom);

// Склейка картинок в сетку с подписями.
QImage composeSheet(const std::vector<QString> &columnTitles,
                    const std::vector<QString> &rowTitles,
                    const std::vector<std::vector<QImage>> &cells, int zoom);

QImage upscale(const QImage &image, int zoom);

} // namespace lab
