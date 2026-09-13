#pragma once

#include <QQuickItem>
#include <QStringList>

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

signals:
    void logChanged();
    void summaryChanged();

protected:
    bool event(QEvent *event) override;

private:
    void record(const QString &line);
    void note(const QPointerEvent *event, const QEventPoint &point, const QString &kind);

    QStringList m_log;

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
