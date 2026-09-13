#include "probeitem.h"

#include <QEventPoint>
#include <QMetaEnum>
#include <QPointerEvent>
#include <QPointingDevice>
#include <QTextStream>

namespace {

QString deviceTypeName(const QInputDevice *device)
{
    if (!device)
        return QStringLiteral("<no device>");
    switch (device->type()) {
    case QInputDevice::DeviceType::Mouse: return QStringLiteral("Mouse");
    case QInputDevice::DeviceType::TouchScreen: return QStringLiteral("TouchScreen");
    case QInputDevice::DeviceType::TouchPad: return QStringLiteral("TouchPad");
    case QInputDevice::DeviceType::Stylus: return QStringLiteral("Stylus");
    case QInputDevice::DeviceType::Airbrush: return QStringLiteral("Airbrush");
    case QInputDevice::DeviceType::Puck: return QStringLiteral("Puck");
    case QInputDevice::DeviceType::Keyboard: return QStringLiteral("Keyboard");
    case QInputDevice::DeviceType::Unknown: return QStringLiteral("Unknown");
    default: return QStringLiteral("DeviceType(%1)").arg(int(device->type()));
    }
}

QString pointerTypeName(const QPointingDevice *device)
{
    if (!device)
        return QStringLiteral("<no device>");
    switch (device->pointerType()) {
    case QPointingDevice::PointerType::Generic: return QStringLiteral("Generic");
    case QPointingDevice::PointerType::Finger: return QStringLiteral("Finger");
    case QPointingDevice::PointerType::Pen: return QStringLiteral("Pen");
    case QPointingDevice::PointerType::Eraser: return QStringLiteral("Eraser");
    case QPointingDevice::PointerType::Cursor: return QStringLiteral("Cursor");
    case QPointingDevice::PointerType::Unknown: return QStringLiteral("Unknown");
    default: return QStringLiteral("PointerType(%1)").arg(int(device->pointerType()));
    }
}

// Кнопки на корпусе пера: у этого устройства второго конца нет, и ластик
// придётся вешать на кнопку, поэтому надо точно знать, чем она приходит.
QString buttonNames(Qt::MouseButtons buttons)
{
    if (buttons == Qt::NoButton)
        return QStringLiteral("none");
    QStringList names;
    if (buttons & Qt::LeftButton) names << QStringLiteral("Left");
    if (buttons & Qt::RightButton) names << QStringLiteral("Right");
    if (buttons & Qt::MiddleButton) names << QStringLiteral("Middle");
    if (buttons & Qt::BackButton) names << QStringLiteral("Back");
    if (buttons & Qt::ForwardButton) names << QStringLiteral("Forward");
    if (buttons & Qt::TaskButton) names << QStringLiteral("Task");
    if (names.isEmpty())
        names << QStringLiteral("0x%1").arg(quint32(buttons), 0, 16);
    return names.join(QLatin1Char('+'));
}

QString eventKind(QEvent::Type type)
{
    return QString::fromLatin1(QMetaEnum::fromType<QEvent::Type>().valueToKey(int(type)));
}

QString gestureTypeName(Qt::NativeGestureType type)
{
    switch (type) {
    case Qt::BeginNativeGesture: return QStringLiteral("Begin");
    case Qt::EndNativeGesture: return QStringLiteral("End");
    case Qt::PanNativeGesture: return QStringLiteral("Pan");
    case Qt::ZoomNativeGesture: return QStringLiteral("Zoom");
    case Qt::SmartZoomNativeGesture: return QStringLiteral("SmartZoom");
    case Qt::RotateNativeGesture: return QStringLiteral("Rotate");
    case Qt::SwipeNativeGesture: return QStringLiteral("Swipe");
    }
    return QStringLiteral("NativeGesture(%1)").arg(int(type));
}

} // namespace

ProbeItem::ProbeItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptTouchEvents(true);
    setAcceptHoverEvents(true);
}

void ProbeItem::reset()
{
    m_log.clear();
    m_stylusEvents = m_eraserEvents = m_touchEvents = m_mouseEvents = 0;
    m_buttonEvents = 0;
    m_stylusTabletEvents = m_stylusMouseEvents = 0;
    m_buttonsSeen.clear();
    m_pressureSamples = m_distinctPressureValues = 0;
    m_minPressure = 2.0;
    m_maxPressure = -1.0;
    m_lastPressure = -1.0;
    m_pressureOutOfRange = false;
    m_gestureSequences = m_zoomEvents = m_panEvents = m_rotateEvents = m_otherGestureEvents = 0;
    m_zoomSum = 0.0;
    m_panSum = {};
    m_hasFinishedGesture = false;
    m_lastGestureZoomSum = 0.0;
    m_lastGesturePanSum = {};
    emit logChanged();
    emit summaryChanged();
}

void ProbeItem::record(const QString &line)
{
    QTextStream(stdout) << line << Qt::endl;
    m_log.append(line);
    while (m_log.size() > 200)
        m_log.removeFirst();
    emit logChanged();
}

void ProbeItem::note(const QPointerEvent *event, const QEventPoint &point, const QString &kind)
{
    const auto *device = event->pointingDevice();
    const QString type = deviceTypeName(device);
    const QString pointer = pointerTypeName(device);
    const qreal pressure = point.pressure();

    if (device) {
        switch (device->type()) {
        case QInputDevice::DeviceType::Stylus:
        case QInputDevice::DeviceType::Airbrush:
        case QInputDevice::DeviceType::Puck:
            ++m_stylusEvents;
            if (device->pointerType() == QPointingDevice::PointerType::Eraser)
                ++m_eraserEvents;
            break;
        case QInputDevice::DeviceType::TouchScreen:
        case QInputDevice::DeviceType::TouchPad:
            ++m_touchEvents;
            break;
        case QInputDevice::DeviceType::Mouse:
            ++m_mouseEvents;
            break;
        default:
            break;
        }

        const bool stylus = device->type() == QInputDevice::DeviceType::Stylus
            || device->type() == QInputDevice::DeviceType::Airbrush;
        if (stylus) {
            if (kind.startsWith(QLatin1String("Tablet")))
                ++m_stylusTabletEvents;
            else if (kind.startsWith(QLatin1String("Mouse")))
                ++m_stylusMouseEvents;
        }
        if (stylus && point.state() != QEventPoint::State::Released) {
            ++m_pressureSamples;
            if (pressure < 0.0 || pressure > 1.0)
                m_pressureOutOfRange = true;
            m_minPressure = qMin(m_minPressure, double(pressure));
            m_maxPressure = qMax(m_maxPressure, double(pressure));
            if (!qFuzzyCompare(m_lastPressure + 1.0, pressure + 1.0))
                ++m_distinctPressureValues;
            m_lastPressure = pressure;
        }
    }

    record(QStringLiteral("%1 dev=%2 pointer=%3 p=%4 rot=%5 pos=%6,%7 id=%8")
               .arg(kind, type, pointer)
               .arg(pressure, 0, 'f', 4)
               .arg(point.rotation(), 0, 'f', 1)
               .arg(point.position().x(), 0, 'f', 1)
               .arg(point.position().y(), 0, 'f', 1)
               .arg(device ? device->systemId() : 0));
    emit summaryChanged();
}

void ProbeItem::noteGesture(const QNativeGestureEvent *event)
{
    const auto *device = event->pointingDevice();
    switch (event->gestureType()) {
    case Qt::BeginNativeGesture:
        ++m_gestureSequences;
        m_zoomSum = 0.0;
        m_panSum = {};
        break;
    case Qt::ZoomNativeGesture:
        ++m_zoomEvents;
        m_zoomSum += event->value();
        break;
    case Qt::PanNativeGesture:
        ++m_panEvents;
        m_panSum += event->delta();
        break;
    case Qt::RotateNativeGesture:
        ++m_rotateEvents;
        break;
    case Qt::EndNativeGesture:
        m_hasFinishedGesture = true;
        m_lastGestureZoomSum = m_zoomSum;
        m_lastGesturePanSum = m_panSum;
        break;
    default:
        ++m_otherGestureEvents;
        break;
    }

    record(QStringLiteral("NativeGesture %1 dev=%2 pointer=%3 value=%4 delta=%5,%6 pos=%7,%8 "
                          "fingers=%9 id=%10")
               .arg(gestureTypeName(event->gestureType()), deviceTypeName(device),
                    pointerTypeName(device))
               .arg(event->value(), 0, 'f', 5)
               .arg(event->delta().x(), 0, 'f', 2)
               .arg(event->delta().y(), 0, 'f', 2)
               .arg(event->position().x(), 0, 'f', 1)
               .arg(event->position().y(), 0, 'f', 1)
               .arg(event->fingerCount())
               .arg(device ? device->systemId() : 0));
    if (event->gestureType() == Qt::EndNativeGesture) {
        record(QStringLiteral("    gesture total: 1 + sum(zoom value)=%1 sum(pan delta)=%2,%3")
                   .arg(1.0 + m_zoomSum, 0, 'f', 4)
                   .arg(m_panSum.x(), 0, 'f', 1)
                   .arg(m_panSum.y(), 0, 'f', 1));
    }
    emit summaryChanged();
}

bool ProbeItem::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::NativeGesture: {
        auto *gesture = static_cast<QNativeGestureEvent *>(event);
        noteGesture(gesture);
        gesture->accept();
        return true;
    }
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity: {
        auto *tablet = static_cast<QTabletEvent *>(event);
        const auto &point = tablet->points().first();
        note(tablet, point, eventKind(event->type()));

        const QString buttons = buttonNames(tablet->buttons());
        if (tablet->buttons() != Qt::NoButton) {
            ++m_buttonEvents;
            if (!m_buttonsSeen.contains(buttons))
                m_buttonsSeen.append(buttons);
        }
        record(QStringLiteral("    tilt xTilt=%1 yTilt=%2 tangential=%3 buttons=%4")
                   .arg(tablet->xTilt())
                   .arg(tablet->yTilt())
                   .arg(tablet->tangentialPressure(), 0, 'f', 3)
                   .arg(buttons));
        tablet->accept();
        return true;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto *touch = static_cast<QTouchEvent *>(event);
        for (const QEventPoint &point : touch->points())
            note(touch, point, eventKind(event->type()));
        touch->accept();
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        // Событие мыши может нести устройство Stylus: Qt Quick доставляет перо
        // элементу именно так, сохраняя давление и тип указателя.
        note(mouse, mouse->points().first(),
             eventKind(event->type())
                 + (mouse->source() == Qt::MouseEventNotSynthesized ? QString()
                                                                    : QStringLiteral("[synth]")));

        const QString buttons = buttonNames(mouse->buttons());
        if (mouse->buttons() != Qt::NoButton) {
            ++m_buttonEvents;
            if (!m_buttonsSeen.contains(buttons))
                m_buttonsSeen.append(buttons);
        }
        record(QStringLiteral("    buttons=%1 button=%2 source=%3")
                   .arg(buttons)
                   .arg(buttonNames(mouse->button()))
                   .arg(int(mouse->source())));
        mouse->accept();
        return true;
    }
    default:
        break;
    }
    return QQuickItem::event(event);
}

QString ProbeItem::summary() const
{
    QString out;
    QTextStream stream(&out);
    stream << "stylus events: " << m_stylusEvents << " (eraser: " << m_eraserEvents << ")\n"
           << "  of them as tablet events: " << m_stylusTabletEvents
           << ", as mouse events: " << m_stylusMouseEvents << "\n"
           << "touch events:  " << m_touchEvents << "\n"
           << "mouse events:  " << m_mouseEvents << "\n"
           << "pressure samples: " << m_pressureSamples
           << ", distinct values: " << m_distinctPressureValues << "\n";
    if (m_pressureSamples > 0) {
        stream << "pressure range: " << QString::number(m_minPressure, 'f', 4) << " .. "
               << QString::number(m_maxPressure, 'f', 4)
               << (m_pressureOutOfRange ? "  OUT OF 0..1 RANGE" : "  (within 0..1)") << "\n";
    }
    stream << "button events: " << m_buttonEvents
           << ", buttons seen: " << (m_buttonsSeen.isEmpty() ? QStringLiteral("(none)")
                                                             : m_buttonsSeen.join(", "))
           << "\n";
    stream << "native gestures: " << m_gestureSequences << " (zoom: " << m_zoomEvents
           << ", pan: " << m_panEvents << ", rotate: " << m_rotateEvents
           << ", other: " << m_otherGestureEvents << ")\n";
    if (m_hasFinishedGesture) {
        stream << "last gesture: 1 + sum(zoom value) = "
               << QString::number(1.0 + m_lastGestureZoomSum, 'f', 4)
               << ", sum(pan delta) = " << QString::number(m_lastGesturePanSum.x(), 'f', 1) << ","
               << QString::number(m_lastGesturePanSum.y(), 'f', 1) << "\n";
    }
    stream << "\nVERDICT\n"
           << "  pressure continuous: "
           << (m_distinctPressureValues > 10 ? "YES" : "NO - fallback to libinput") << "\n"
           << "  eraser end reported: "
           << (m_eraserEvents > 0 ? "YES" : "NOT SEEN") << "\n"
           << "  barrel button reported: "
           << (m_buttonEvents > 0 ? "YES" : "NOT SEEN - hold the pen button and draw") << "\n"
           << "  stylus delivered as: "
           << (m_stylusTabletEvents > 0 ? "TabletEvent"
                                        : (m_stylusMouseEvents > 0 ? "MouseEvent with Stylus device"
                                                                   : "NOT SEEN"))
           << "\n"
           << "  native gesture reaches item: "
           << (m_gestureSequences + m_zoomEvents + m_panEvents > 0
                   ? "YES"
                   : "NOT SEEN - pinch two fingers on the touchpad")
           << "\n"
           << "  pressure usable from delivered events: "
           << (m_pressureSamples > 0 ? "YES" : "NO - fallback to libinput") << "\n";
    return out;
}

void ProbeItem::dumpSummary() const
{
    QTextStream(stdout) << "\n=== SPIKE 1.1 SUMMARY ===\n" << summary() << Qt::endl;
}
