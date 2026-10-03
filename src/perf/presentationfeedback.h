#pragma once

#include <QMutex>
#include <QObject>
#include <QSet>

#include <atomic>
#include <cstdint>

#include "presentationstats.h"

class QQuickWindow;
struct wl_display;
struct wl_event_queue;
struct wl_output;
struct wl_registry;
struct wl_surface;
struct wp_presentation;
struct wp_presentation_feedback;

// Задержка «метка события пера -> показ кадра на экране» по протоколу
// wp_presentation (design.md, D1). FrameStats меряет только до frameSwapped,
// а кадр после этого ещё ждёт композитор и сканирование панели; именно этот
// хвост решает, нужен ли предсказатель кончика.
//
// Создаётся только при UQ_FRAME_LOG и только на Wayland (specs/idle-power:
// без явного запроса замер не стоит ничего). На любой другой платформе, при
// композиторе без wp_presentation и в сборке без wayland-client create()
// возвращает nullptr, и FrameStats работает как раньше.
//
// Потоки. noteInput() и dispatchPending() - поток GUI. Кадр собирается на
// потоке рендера: в beforeSynchronizing (поток GUI в этот момент стоит)
// забираются события, вошедшие в кадр, в afterRendering (до swap, то есть до
// wl_surface.commit этого кадра) на них заказывается feedback. Ответы
// композитора приходят на собственную wl_event_queue и разбираются на потоке
// GUI, очередь Qt не трогается.
class PresentationFeedback : public QObject
{
    Q_OBJECT

public:
    // Окно должно быть уже показано: wl_surface появляется вместе с
    // платформенным окном, поэтому FrameStats зовёт create() на первом
    // показанном кадре, а не при setWindow().
    static PresentationFeedback *create(QQuickWindow *window, QObject *parent);
    ~PresentationFeedback() override;

    // Событие с меткой eventNs (FrameStats::nowNs()) обработано и войдёт в
    // ближайший синхронизированный кадр.
    void noteInput(qint64 eventNs);
    // Разбирает ответы, уже прочитанные из сокета потоком событий Qt. Своего
    // нотификатора нет: зовётся из frameSwapped и раз в секунду из журнала.
    void dispatchPending();
    PresentationSummary takeSummary() { return m_stats.takeSummary(); }

private:
    struct Request
    {
        PresentationFeedback *owner = nullptr;
        wp_presentation_feedback *feedback = nullptr;
        FrameInputs inputs;
    };

    PresentationFeedback(QQuickWindow *window, QObject *parent);
    bool init();
    void onBeforeSynchronizing();
    void onAfterRendering();
    void finish(Request *request);

    static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                               const char *interface, uint32_t version);
    static void registryGlobalRemove(void *data, wl_registry *registry, uint32_t name);
    static void presentationClockId(void *data, wp_presentation *presentation, uint32_t clockId);
    static void feedbackSyncOutput(void *data, wp_presentation_feedback *feedback,
                                   wl_output *output);
    static void feedbackPresented(void *data, wp_presentation_feedback *feedback,
                                  uint32_t secondsHigh, uint32_t secondsLow, uint32_t nanoseconds,
                                  uint32_t refresh, uint32_t sequenceHigh, uint32_t sequenceLow,
                                  uint32_t flags);
    static void feedbackDiscarded(void *data, wp_presentation_feedback *feedback);

    QQuickWindow *m_window = nullptr;
    wl_display *m_display = nullptr;
    wl_display *m_displayWrapper = nullptr;
    wl_event_queue *m_queue = nullptr;
    wl_registry *m_registry = nullptr;
    wp_presentation *m_presentation = nullptr;
    wl_surface *m_surface = nullptr;
    uint32_t m_clockId = 1; // CLOCK_MONOTONIC, пока композитор не сказал иного

    // Поток GUI пишет, поток рендера забирает на синхронизации.
    std::atomic<qint64> m_pendingOldestNs{0};
    std::atomic<qint64> m_pendingNewestNs{0};
    // Только поток рендера: от синхронизации до afterRendering того же кадра.
    FrameInputs m_frameInputs;

    QMutex m_mutex; // m_requests: создаются на потоке рендера, гаснут на GUI
    QSet<Request *> m_requests;
    PresentationStats m_stats; // только поток GUI
};
