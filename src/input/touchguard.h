#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSet>

class QPointingDevice;
class QTouchEvent;

// Режим «Только перо» (specs/pen-only-mode/spec.md): пока он включён, касания
// сенсорного экрана не нажимают элементы окна, а навигация пальцами - сдвиг и
// pinch на холсте, прокрутка оглавления - работает. Перо, мышь и тачпад
// проходят как есть.
//
// Фильтр стоит на окне и решает судьбу каждой точки касания по месту, где она
// коснулась экрана: области навигации размечены в QML свойством
// touchNavigation, всё остальное навигацией не считается (design.md, D1-D3).
class TouchGuard : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool penOnly READ penOnly WRITE setPenOnly NOTIFY penOnlyChanged)

public:
    explicit TouchGuard(QObject *parent = nullptr);
    ~TouchGuard() override;

    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);

    bool penOnly() const { return m_penOnly; }
    void setPenOnly(bool penOnly);

    // Разрешает ли место касания навигацию: самый верхний видимый элемент под
    // точкой и ближайший из его предков, у которого объявлено свойство
    // touchNavigation. Нет такого предка - навигации нет.
    static bool allowsNavigation(QQuickWindow *window, const QPointF &scenePoint);

signals:
    void windowChanged();
    void penOnlyChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool filterTouch(QTouchEvent *event);

    QPointer<QQuickWindow> m_window;
    bool m_penOnly = false;
    // Пересобранное событие уходит окну через этот же фильтр (D4).
    bool m_forwarding = false;

    // Точки, нажатые в режиме вне областей навигации. Каждая отклоняется до
    // своего отрыва, даже если режим за это время выключили: иначе Qt Quick
    // получил бы отрыв точки без её нажатия (D2).
    QHash<const QPointingDevice *, QSet<int>> m_blocked;
};
