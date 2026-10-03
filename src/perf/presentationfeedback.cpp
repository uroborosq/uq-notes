#include "presentationfeedback.h"

#include <QGuiApplication>
#include <QMutexLocker>
#include <QQuickWindow>

#ifdef UQ_HAVE_WP_PRESENTATION
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>

#include "presentation-time-client-protocol.h"

#include <cstring>
#include <ctime>
#endif

namespace {

#ifdef UQ_HAVE_WP_PRESENTATION
qint64 clockNs(clockid_t clock)
{
    timespec ts{};
    clock_gettime(clock, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Время показа приходит в часах, которые назвал композитор (clock_id). KWin,
// Mutter и Weston называют CLOCK_MONOTONIC - ту же шкалу, что у
// FrameStats::nowNs() (std::chrono::steady_clock в libstdc++). Для любых других
// часов разница шкал берётся в момент ответа: ошибка - доли микросекунды.
qint64 toMonotonicNs(qint64 ns, uint32_t clockId)
{
    if (clockId == uint32_t(CLOCK_MONOTONIC))
        return ns;
    return ns - clockNs(clockid_t(clockId)) + clockNs(CLOCK_MONOTONIC);
}
#endif

} // namespace

PresentationFeedback *PresentationFeedback::create(QQuickWindow *window, QObject *parent)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    if (!window || !QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        return nullptr;
    auto *feedback = new PresentationFeedback(window, parent);
    if (!feedback->init()) {
        delete feedback;
        return nullptr;
    }
    return feedback;
#else
    Q_UNUSED(window);
    Q_UNUSED(parent);
    return nullptr;
#endif
}

PresentationFeedback::PresentationFeedback(QQuickWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
}

PresentationFeedback::~PresentationFeedback()
{
    // Поток рендера к этому моменту уже не зовёт нас: FrameStats уничтожается
    // вместе с окном после остановки рендера или удаляет нас сам при смене окна,
    // а отключение ниже снимает прямые соединения.
    if (m_window)
        m_window->disconnect(this);
#ifdef UQ_HAVE_WP_PRESENTATION
    {
        QMutexLocker lock(&m_mutex);
        for (Request *request : std::as_const(m_requests)) {
            wp_presentation_feedback_destroy(request->feedback);
            delete request;
        }
        m_requests.clear();
    }
    if (m_presentation)
        wp_presentation_destroy(m_presentation);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_displayWrapper)
        wl_proxy_wrapper_destroy(m_displayWrapper);
    if (m_queue)
        wl_event_queue_destroy(m_queue);
#endif
}

bool PresentationFeedback::init()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display())
        return false;
    m_display = wayland->display();
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    m_surface = native ? static_cast<wl_surface *>(
                    native->nativeResourceForWindow(QByteArrayLiteral("surface"), m_window))
                       : nullptr;
    if (!m_surface)
        return false;

    // Своя очередь: ответы композитора не попадают в очередь Qt и разбираются
    // тогда, когда нам удобно, а не на потоке событий Qt.
    m_queue = wl_display_create_queue(m_display);
    m_displayWrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_displayWrapper), m_queue);
    m_registry = wl_display_get_registry(m_displayWrapper);
    static const wl_registry_listener registryListener = {
        &PresentationFeedback::registryGlobal,
        &PresentationFeedback::registryGlobalRemove,
    };
    wl_registry_add_listener(m_registry, &registryListener, this);
    if (wl_display_roundtrip_queue(m_display, m_queue) < 0 || !m_presentation)
        return false;

    static const wp_presentation_listener presentationListener = {
        &PresentationFeedback::presentationClockId,
    };
    wp_presentation_add_listener(m_presentation, &presentationListener, this);
    // Второй обмен - за clock_id, который композитор шлёт сразу после bind.
    if (wl_display_roundtrip_queue(m_display, m_queue) < 0)
        return false;

    connect(m_window, &QQuickWindow::beforeSynchronizing, this,
            &PresentationFeedback::onBeforeSynchronizing, Qt::DirectConnection);
    connect(m_window, &QQuickWindow::afterRendering, this,
            &PresentationFeedback::onAfterRendering, Qt::DirectConnection);
    return true;
#else
    return false;
#endif
}

void PresentationFeedback::noteInput(qint64 eventNs)
{
    if (eventNs <= 0)
        return;
    qint64 expected = 0;
    m_pendingOldestNs.compare_exchange_strong(expected, eventNs, std::memory_order_relaxed);
    m_pendingNewestNs.store(eventNs, std::memory_order_relaxed);
}

void PresentationFeedback::onBeforeSynchronizing()
{
    // Поток GUI стоит на синхронизации, поэтому всё, что он успел отметить,
    // войдёт ровно в этот кадр, а отмеченное позже - в следующий.
    m_frameInputs = FrameInputs();
    m_frameInputs.add(m_pendingOldestNs.exchange(0, std::memory_order_relaxed));
    m_frameInputs.add(m_pendingNewestNs.exchange(0, std::memory_order_relaxed));
}

void PresentationFeedback::onAfterRendering()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    // Кадры без ввода не заказываются: их задержка ничего не говорит о пере, а
    // лишний объект протокола на каждый кадр - лишняя работа композитора.
    if (m_frameInputs.isEmpty())
        return;
    auto *request = new Request;
    request->owner = this;
    request->inputs = m_frameInputs;
    m_frameInputs = FrameInputs();
    // feedback относится к ближайшему wl_surface.commit, а его делает
    // eglSwapBuffers сразу после этого сигнала. Ответ придёт не раньше показа
    // кадра, то есть заведомо после add_listener ниже.
    request->feedback = wp_presentation_feedback(m_presentation, m_surface);
    static const wp_presentation_feedback_listener feedbackListener = {
        &PresentationFeedback::feedbackSyncOutput,
        &PresentationFeedback::feedbackPresented,
        &PresentationFeedback::feedbackDiscarded,
    };
    wp_presentation_feedback_add_listener(request->feedback, &feedbackListener, request);
    QMutexLocker lock(&m_mutex);
    m_requests.insert(request);
#endif
}

void PresentationFeedback::dispatchPending()
{
#ifdef UQ_HAVE_WP_PRESENTATION
    if (m_display && m_queue)
        wl_display_dispatch_queue_pending(m_display, m_queue);
#endif
}

void PresentationFeedback::finish(Request *request)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    wp_presentation_feedback_destroy(request->feedback);
    {
        QMutexLocker lock(&m_mutex);
        m_requests.remove(request);
    }
    delete request;
#else
    Q_UNUSED(request);
#endif
}

void PresentationFeedback::registryGlobal(void *data, wl_registry *registry, uint32_t name,
                                          const char *interface, uint32_t version)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *self = static_cast<PresentationFeedback *>(data);
    if (!self->m_presentation && std::strcmp(interface, wp_presentation_interface.name) == 0) {
        // Версии 1 достаточно: presented и discarded в ней уже есть.
        Q_UNUSED(version);
        self->m_presentation = static_cast<wp_presentation *>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
    }
#else
    Q_UNUSED(data);
    Q_UNUSED(registry);
    Q_UNUSED(name);
    Q_UNUSED(interface);
    Q_UNUSED(version);
#endif
}

void PresentationFeedback::registryGlobalRemove(void *, wl_registry *, uint32_t)
{
}

void PresentationFeedback::presentationClockId(void *data, wp_presentation *, uint32_t clockId)
{
    static_cast<PresentationFeedback *>(data)->m_clockId = clockId;
}

void PresentationFeedback::feedbackSyncOutput(void *, struct wp_presentation_feedback *, wl_output *)
{
}

void PresentationFeedback::feedbackPresented(void *data, struct wp_presentation_feedback *,
                                             uint32_t secondsHigh, uint32_t secondsLow,
                                             uint32_t nanoseconds, uint32_t, uint32_t, uint32_t,
                                             uint32_t)
{
#ifdef UQ_HAVE_WP_PRESENTATION
    auto *request = static_cast<Request *>(data);
    PresentationFeedback *self = request->owner;
    const qint64 seconds = qint64((quint64(secondsHigh) << 32) | secondsLow);
    const qint64 presentedNs =
        toMonotonicNs(seconds * 1'000'000'000 + qint64(nanoseconds), self->m_clockId);
    self->m_stats.addPresented(request->inputs, presentedNs);
    self->finish(request);
#else
    Q_UNUSED(data);
    Q_UNUSED(secondsHigh);
    Q_UNUSED(secondsLow);
    Q_UNUSED(nanoseconds);
#endif
}

void PresentationFeedback::feedbackDiscarded(void *data, struct wp_presentation_feedback *)
{
    auto *request = static_cast<Request *>(data);
    request->owner->m_stats.addDiscarded();
    request->owner->finish(request);
}
