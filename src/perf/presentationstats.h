#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>

// Задержка «метка события пера -> показ кадра с этим событием» (design.md, D1).
// Здесь только арифметика: какие события вошли в кадр и как свести задержки в
// сводку. Протокол wp_presentation живёт в presentationfeedback.cpp и
// проверяется руками на устройстве; эта часть проверяется tst_framestats без
// дисплея.
//
// Все метки - наносекунды шкалы FrameStats::nowNs() (CLOCK_MONOTONIC).

// События ввода, вошедшие в один кадр. Кадр забирает всё, что поток GUI успел
// обработать до синхронизации сцены, поэтому событий в нём обычно несколько
// (перо 481 Гц, кадр 120 Гц): самое раннее даёт худшую задержку кадра, самое
// позднее - лучшую.
struct FrameInputs
{
    qint64 oldestNs = 0;
    qint64 newestNs = 0;

    bool isEmpty() const { return oldestNs <= 0; }
    void add(qint64 eventNs);
};

// Какие события ввода вошли в кадр. Поток GUI отмечает события (noteInput),
// поток рендера на синхронизации забирает всё отмеченное в кадр (beginFrame:
// поток GUI в этот момент стоит), после отрисовки отдаёт заказу feedback
// (takeForRenderedFrame) и на конце кадра сбрасывает остаток (endFrame). Так
// события кадра, который закончился без отрисовки (beginFrame RHI не удался,
// окно не готово), не приписываются следующему кадру: Qt 6 парно шлёт
// beforeFrameBegin/afterFrameEnd в обоих циклах рендера, в том числе на этих
// путях.
class FrameInputTracker
{
public:
    void noteInput(qint64 eventNs); // поток GUI
    void beginFrame();              // beforeSynchronizing, поток рендера
    FrameInputs takeForRenderedFrame(); // afterRendering, поток рендера
    void endFrame() { m_frame = FrameInputs(); } // afterFrameEnd, поток рендера

private:
    // Поток GUI пишет, поток рендера забирает на синхронизации.
    std::atomic<qint64> m_pendingOldestNs{0};
    std::atomic<qint64> m_pendingNewestNs{0};
    // Только поток рендера: от синхронизации до конца того же кадра.
    FrameInputs m_frame;
};

struct PresentationSummary
{
    int presented = 0; // кадров с вводом, о показе которых пришёл ответ
    int discarded = 0; // кадров с вводом, которые композитор так и не показал
    int rejected = 0;  // показ раньше события: шкалы часов не совпали
    qreal oldestMedianMs = 0.0;
    qreal oldestP95Ms = 0.0;
    qreal newestMedianMs = 0.0;
    qreal maxMs = 0.0;

    bool isEmpty() const { return presented == 0 && discarded == 0 && rejected == 0; }
    QString toString() const;
};

class PresentationStats
{
public:
    // Кадр с событиями inputs показан на экране в presentedNs.
    void addPresented(const FrameInputs &inputs, qint64 presentedNs);
    void addDiscarded() { ++m_discarded; }
    // Сводка с прошлого вызова; накопленное сбрасывается, чтобы журнал раз в
    // секунду показывал именно эту секунду, а не среднее за весь запуск.
    PresentationSummary takeSummary();

private:
    QVector<qreal> m_oldestMs;
    QVector<qreal> m_newestMs;
    int m_discarded = 0;
    int m_rejected = 0;
};
