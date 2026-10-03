#pragma once

#include <QJsonObject>
#include <QPointF>

#include <memory>
#include <vector>

#include "corpus.h"
#include "lab.h"

namespace lab {

// Что лаборатория записала про один штрих одного варианта. Всё - в ЭКРАННЫХ
// логических пикселях, кроме centerline (координаты страницы, как у варианта).
struct EventRecord
{
    QPointF tip;                        // конец мокрой линии
    std::vector<QPointF> provisional;   // провизорный хвост
    double cpuMicros = 0.0;             // addEvent, минимум по повторам
    int wetDirtyVertices = 0;           // vertexCount - settledVertexCount
};

struct StrokeRun
{
    std::vector<CenterPoint> centerline; // координаты страницы
    std::vector<EventRecord> events;
    double finishMicros = 0.0;
    int finalVertices = 0;
    int settledMutations = 0;
    Geometry geometry;                   // сухая геометрия, координаты страницы
    std::unique_ptr<Variant> variant;    // для своей растеризации

    // Снимок мокрого кадра (только у штриха, выбранного для снимка).
    int wetSnapshotEvent = -1;
    Geometry wetGeometry;
};

struct MetricsOptions
{
    double width = 1.0;   // толщина пера, точки страницы (для эталонной толщины)
    double minWidthFactor = 0.25;
};

QJsonObject computeMetrics(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                           const MetricsOptions &options);

// Эталонная полная толщина в экранных px по давлению (закон приложения).
double truthWidthPx(double pressure, double scale, const MetricsOptions &options);

} // namespace lab
