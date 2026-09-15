#include "touchguard.h"

#include <QCoreApplication>
#include <QPointerEvent>
#include <QQuickItem>

#include <algorithm>

namespace {

constexpr const char *kNavigationMarker = "touchNavigation";

// Самый верхний видимый элемент под точкой. Дети обходятся в порядке z сверху
// вниз, и попадание в родителя не требуется: делегаты Repeater у вкладышей
// нулевого размера, а их элементы лежат за пределами делегата.
QQuickItem *topmostItemAt(QQuickItem *item, const QPointF &scenePoint)
{
    if (!item->isVisible())
        return nullptr;

    QList<QQuickItem *> children = item->childItems();
    std::stable_sort(children.begin(), children.end(),
                     [](QQuickItem *a, QQuickItem *b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        if (QQuickItem *hit = topmostItemAt(*it, scenePoint))
            return hit;
    }

    // Оверлей попапов растянут на всё окно и видим, пока открыт хоть какой-то
    // попап - даже подсказка. Сам по себе он элементом не считается, попадание
    // в попап находится через его потомков (D3).
    if (item->inherits("QQuickOverlay"))
        return nullptr;
    return item->contains(item->mapFromScene(scenePoint)) ? item : nullptr;
}

} // namespace

TouchGuard::TouchGuard(QObject *parent)
    : QObject(parent)
{
}

TouchGuard::~TouchGuard()
{
    if (m_window)
        m_window->removeEventFilter(this);
}

void TouchGuard::setWindow(QQuickWindow *window)
{
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);

    m_window = window;
    m_blocked.clear();

    if (m_window)
        m_window->installEventFilter(this);
    emit windowChanged();
}

void TouchGuard::setPenOnly(bool penOnly)
{
    if (m_penOnly == penOnly)
        return;
    // Идущие касания не трогаются: каждое подчиняется режиму, каким он был в
    // момент касания (D2).
    m_penOnly = penOnly;
    emit penOnlyChanged();
}

bool TouchGuard::allowsNavigation(QQuickWindow *window, const QPointF &scenePoint)
{
    if (!window)
        return false;
    for (QQuickItem *item = topmostItemAt(window->contentItem(), scenePoint); item;
         item = item->parentItem()) {
        const QVariant marker = item->property(kNavigationMarker);
        if (marker.isValid())
            return marker.toBool();
    }
    return false;
}

bool TouchGuard::eventFilter(QObject *watched, QEvent *event)
{
    if (m_forwarding || watched != m_window || !event->isPointerEvent())
        return false;

    // Перо и палец различаются по устройству, а не по типу события: перо на
    // целевом устройстве приходит мышиным событием (spikes/FINDINGS.md, 1.1).
    // Тачпад тоже Finger, поэтому смотрится тип устройства, а не указателя.
    auto *pointerEvent = static_cast<QPointerEvent *>(event);
    const QPointingDevice *device = pointerEvent->pointingDevice();
    if (!device || device->type() != QInputDevice::DeviceType::TouchScreen)
        return false;

    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        return filterTouch(static_cast<QTouchEvent *>(event));
    default:
        break;
    }

    // Мышиная синтетика от экрана, если её пришлёт сама платформа: места у неё
    // не разобрать по точкам, поэтому в режиме она не проходит вовсе.
    if (!m_penOnly)
        return false;
    pointerEvent->setAccepted(true);
    return true;
}

bool TouchGuard::filterTouch(QTouchEvent *event)
{
    const QPointingDevice *device = event->pointingDevice();
    if (event->type() == QEvent::TouchCancel) {
        m_blocked.remove(device);
        return false;
    }
    if (!m_penOnly && !m_blocked.contains(device))
        return false;

    QSet<int> &blocked = m_blocked[device];
    QList<QEventPoint> passing;
    bool anyBlocked = false;
    for (const QEventPoint &point : event->points()) {
        // Судьба точки решается один раз - когда она коснулась экрана.
        if (point.state() == QEventPoint::State::Pressed && m_penOnly
            && !allowsNavigation(m_window, point.scenePosition())) {
            blocked.insert(point.id());
        }
        if (blocked.contains(point.id())) {
            anyBlocked = true;
            if (point.state() == QEventPoint::State::Released)
                blocked.remove(point.id());
        } else {
            passing.append(point);
        }
    }
    if (blocked.isEmpty())
        m_blocked.remove(device);
    if (!anyBlocked)
        return false;

    // Принятое касание платформа не превращает в мышиное событие.
    event->setAccepted(true);
    if (!passing.isEmpty()) {
        // Ладонь на странице и палец на кнопке в одном событии: навигации
        // достаётся своя точка, кнопке - ничего (D4).
        QTouchEvent filtered(event->type(), device, event->modifiers(), passing);
        filtered.setTimestamp(event->timestamp());
        m_forwarding = true;
        QCoreApplication::sendEvent(m_window, &filtered);
        m_forwarding = false;
    }
    return true;
}
