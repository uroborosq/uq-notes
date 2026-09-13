#pragma once

#include <QColor>
#include <QList>
#include <QPointF>

// Инструмент, которым нанесён штрих.
enum class InkTool {
    Pen,
    Eraser,
};

// Точка траектории. Координаты - в системе координат страницы (точки PDF),
// а не экрана: чернила обязаны держаться содержимого страницы при любом
// масштабе и режиме отображения.
struct InkPoint
{
    QPointF position;
    // Давление в диапазоне 0..1. Отрицательное значение означает «устройство
    // давления не сообщает» - тогда штрих рисуется постоянной толщиной.
    qreal pressure = -1.0;
    // Монотонное время события в секундах: нужно фильтру One Euro, который
    // работает по реальным интервалам, а не по номерам событий.
    qreal timeSeconds = 0.0;

    bool hasPressure() const { return pressure >= 0.0; }
};

// Завершённый штрих: то, что кладётся в постоянную геометрию страницы и в файл
// заметки.
struct Stroke
{
    int page = -1;
    InkTool tool = InkTool::Pen;
    QColor color = QColor(20, 24, 40);
    // Толщина при полном нажиме, в точках страницы.
    qreal width = 2.4;
    QList<InkPoint> points;

    bool isValid() const { return page >= 0 && points.size() >= 2; }
};
