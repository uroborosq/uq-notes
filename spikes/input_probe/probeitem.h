#pragma once

#include <QMap>
#include <QQuickItem>
#include <QStringList>

class QMouseEvent;
class QNativeGestureEvent;

// Единственная точка входа для всех указателей: печатает тип устройства,
// pointerType и давление для каждого события, а на выходе сводит статистику,
// по которой видно, годится ли путь через Qt или нужен запасной через libinput.
class ProbeItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged)

public:
    explicit ProbeItem(QQuickItem *parent = nullptr);

    QStringList log() const { return m_log; }
    QString summary() const;

    Q_INVOKABLE void reset();
    Q_INVOKABLE void dumpSummary() const;

    // Режим подсчёта частоты: построчный журнал не пишется. Журнал на каждое
    // событие сам тормозит цикл событий, и тогда замер меряет не доставку
    // отсчётов пера, а скорость ListView.
    static void setRateMode(bool enabled);

signals:
    void logChanged();
    void summaryChanged();

protected:
    bool event(QEvent *event) override;

private:
    void record(const QString &line);
    void note(const QPointerEvent *event, const QEventPoint &point, const QString &kind);
    void noteGesture(const QNativeGestureEvent *event);
    void noteStylusTiming(const QMouseEvent *event);
    void onFrameSwapped();

    QStringList m_log;

    // Частота отсчётов пера (rnote-grade-stroke-fidelity, задача 1.1). Всё
    // считается только по мышиным событиям от Stylus при касании: именно они
    // доходят до холста на целевом устройстве. Время - QInputEvent::timestamp(),
    // то есть время отсчёта, а не момент обработки.
    bool m_stylusContact = false;
    quint64 m_contactStartTs = 0;
    quint64 m_lastStylusTs = 0;
    int m_contactStrokes = 0;
    qint64 m_contactMoves = 0;
    quint64 m_contactMs = 0;
    int m_duplicateTimestamps = 0;
    int m_backwardTimestamps = 0;
    // Интервал между соседними отсчётами в мс -> число; всё от 20 мс - в одной
    // корзине.
    QMap<int, int> m_intervalHistogram;
    // Сколько перемещений пера пришло между соседними кадрами окна.
    int m_movesSinceFrame = 0;
    QMap<int, int> m_movesPerFrame;

    // Нативные жесты тачпада. По суммам за жест видно, что лежит в value():
    // приращение масштаба (тогда 1 + Σ value сходится с тем, во сколько раз
    // свели пальцы) или абсолютный масштаб жеста (тогда сумма уезжает).
    int m_gestureSequences = 0;
    int m_zoomEvents = 0;
    int m_panEvents = 0;
    int m_rotateEvents = 0;
    int m_otherGestureEvents = 0;
    double m_zoomSum = 0.0;
    QPointF m_panSum;
    bool m_hasFinishedGesture = false;
    double m_lastGestureZoomSum = 0.0;
    QPointF m_lastGesturePanSum;

    int m_stylusEvents = 0;
    int m_eraserEvents = 0;
    int m_touchEvents = 0;
    int m_buttonEvents = 0;
    int m_stylusTabletEvents = 0;
    int m_stylusMouseEvents = 0;
    QStringList m_buttonsSeen;
    int m_mouseEvents = 0;
    int m_pressureSamples = 0;
    int m_distinctPressureValues = 0;
    double m_minPressure = 2.0;
    double m_maxPressure = -1.0;
    double m_lastPressure = -1.0;
    bool m_pressureOutOfRange = false;
};
