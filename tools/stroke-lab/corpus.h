#pragma once

// Синтетический корпус почерка с эталоном.
//
// Всё задаётся в ЭКРАННЫХ логических пикселях: так движется рука, независимо от
// масштаба страницы. Лаборатория переводит отсчёты в точки страницы делением на
// масштаб.

#include <QPointF>
#include <QRectF>

#include <string>
#include <vector>

#include "ink/inktypes.h"

namespace lab {

// Эталонная траектория одного штриха: без дрожи и шума датчика, плотно
// (шаг по времени 0.1 мс).
struct TruePath
{
    std::vector<double> t;        // секунды от начала сэмпла
    std::vector<QPointF> p;       // экранные px
    std::vector<double> pressure; // 0..1
    std::vector<double> speed;    // px/s
};

struct SampleStroke
{
    TruePath truth;
    // Отсчёты пера: экранные px, давление, время события (кратно 1 мс).
    std::vector<InkPoint> events;
    // Настоящее время каждого отсчёта (до округления метки) - для лага.
    std::vector<double> eventTrueTime;
};

struct Sample
{
    std::string id;
    std::string title;
    std::vector<std::string> tags; // reference, word, small, corner, fast, slow, closed
    double size = 0.0;             // характерный размер (x-height), px
    std::vector<SampleStroke> strokes;
    std::vector<QPointF> corners;  // эталонные вершины острых углов, px
    QRectF bounds;                 // охват эталона, px

    bool hasTag(const std::string &tag) const;
};

struct CorpusOptions
{
    double rateHz = 481.0;
    double timestampResolution = 0.001;
    double positionQuantum = 1.0 / 256.0; // wl_fixed
    double noiseSigma = 0.05;             // шум датчика, px
    double pressureNoise = 0.004;
};

std::vector<Sample> buildCorpus(const CorpusOptions &options = {});

} // namespace lab
