#pragma once

#include <QList>
#include <QPointF>
#include <QRandomGenerator>

#include <cmath>
#include <limits>

#include "ink/inktypes.h"

// Эталонные траектории для тестов сглаживания и тесселяции. Отдельный файл,
// потому что одни и те же кривые нужны и фильтру, и тесселятору, и тестам
// экспорта.
namespace reference {

inline QList<InkPoint> fromPositions(const QList<QPointF> &positions, qreal hz = 200.0,
                                     qreal pressure = 0.6)
{
    QList<InkPoint> points;
    points.reserve(positions.size());
    for (int i = 0; i < positions.size(); ++i)
        points.append({positions[i], pressure, i / hz});
    return points;
}

inline QList<QPointF> straightLine(int count = 200, qreal length = 300.0)
{
    QList<QPointF> points;
    for (int i = 0; i < count; ++i)
        points.append(QPointF(length * i / (count - 1), 100.0));
    return points;
}

// Петля: замкнутая окружность, на которой кривизна постоянна и велика.
inline QList<QPointF> loop(int count = 240, qreal radius = 40.0)
{
    QList<QPointF> points;
    for (int i = 0; i < count; ++i) {
        const qreal t = 2.0 * M_PI * i / (count - 1);
        points.append(QPointF(120.0 + radius * std::cos(t), 120.0 + radius * std::sin(t)));
    }
    return points;
}

// Резкие изгибы: зигзаг с разворотом почти на месте - худший случай для стыка.
inline QList<QPointF> sharpZigzag(int teeth = 6, qreal step = 24.0)
{
    QList<QPointF> points;
    QPointF cursor(40.0, 200.0);
    points.append(cursor);
    for (int i = 0; i < teeth; ++i) {
        const qreal dy = (i % 2 == 0) ? -step * 2.4 : step * 2.4;
        for (int s = 1; s <= 8; ++s)
            points.append(cursor + QPointF(step * s / 8.0, dy * s / 8.0));
        cursor = points.constLast();
    }
    return points;
}

// Спираль: кривизна нарастает до предела - проверка скругления на тугих витках.
inline QList<QPointF> tightSpiral(int count = 400)
{
    QList<QPointF> points;
    for (int i = 0; i < count; ++i) {
        const qreal t = 6.0 * M_PI * i / (count - 1);
        const qreal r = 60.0 * (1.0 - qreal(i) / count) + 2.0;
        points.append(QPointF(150.0 + r * std::cos(t), 150.0 + r * std::sin(t)));
    }
    return points;
}

// Рукописная «e»: то, форму чего сглаживание не имеет права испортить.
inline QList<QPointF> letterE(int count = 220)
{
    QList<QPointF> points;
    for (int i = 0; i < count; ++i) {
        const qreal t = qreal(i) / (count - 1);
        // Горизонтальная перекладина, переходящая в петлю.
        const qreal angle = -0.35 * M_PI + t * 1.75 * M_PI;
        const qreal r = 18.0;
        const qreal x = 60.0 + r * std::cos(angle) + t * 6.0;
        const qreal y = 60.0 - r * std::sin(angle);
        points.append(QPointF(x, y));
    }
    return points;
}

// «Кардиограмма»: быстрое движение пера вверх-вниз со сдвигом вбок - то самое
// зюзюкание, на котором видно, срезает ли сглаживание углы. Задаётся в
// ЭКРАННЫХ единицах: именно так её рисует рука, независимо от масштаба.
struct Cardiogram
{
    QList<QPointF> screenPath; // как её ведёт рука
    qreal amplitude = 0.0;     // размах по вертикали, экранные единицы
    qreal hz = 200.0;
};

inline Cardiogram cardiogram(qreal amplitude = 15.0, qreal length = 120.0, int cycles = 12,
                             qreal seconds = 1.2, qreal hz = 200.0)
{
    Cardiogram result;
    result.amplitude = amplitude;
    result.hz = hz;
    const int count = int(seconds * hz);
    for (int i = 0; i < count; ++i) {
        const qreal t = qreal(i) / (count - 1);
        result.screenPath.append(
            QPointF(t * length, amplitude * std::sin(t * cycles * 2.0 * M_PI)));
    }
    return result;
}

// Переводит экранный путь в координаты страницы при заданном масштабе.
inline QList<InkPoint> atScale(const QList<QPointF> &screenPath, qreal scale, qreal hz = 200.0)
{
    QList<InkPoint> points;
    points.reserve(screenPath.size());
    for (int i = 0; i < screenPath.size(); ++i)
        points.append({screenPath[i] / scale, 0.6, i / hz});
    return points;
}

// Размах по вертикали.
inline qreal peakToPeakY(const QList<QPointF> &path)
{
    if (path.isEmpty())
        return 0.0;
    qreal lo = path.first().y();
    qreal hi = lo;
    for (const QPointF &p : path) {
        lo = qMin(lo, p.y());
        hi = qMax(hi, p.y());
    }
    return hi - lo;
}

// Дрожание руки: маленький высокочастотный шум поверх траектории.
inline QList<QPointF> withJitter(const QList<QPointF> &clean, qreal amplitude, quint32 seed)
{
    QRandomGenerator random(seed);
    QList<QPointF> noisy;
    noisy.reserve(clean.size());
    for (const QPointF &point : clean) {
        const auto noise = [&random, amplitude] {
            return (random.generateDouble() * 2.0 - 1.0) * amplitude;
        };
        noisy.append(point + QPointF(noise(), noise()));
    }
    return noisy;
}

namespace detail {

inline QList<qreal> deviations(const QList<QPointF> &path, const QList<QPointF> &reference)
{
    const auto distanceToSegment = [](const QPointF &p, const QPointF &a, const QPointF &b) {
        const QPointF ab = b - a;
        const qreal lengthSquared = QPointF::dotProduct(ab, ab);
        if (lengthSquared < 1e-12)
            return std::hypot(p.x() - a.x(), p.y() - a.y());
        qreal t = QPointF::dotProduct(p - a, ab) / lengthSquared;
        t = qBound(0.0, t, 1.0);
        const QPointF projection = a + ab * t;
        return std::hypot(p.x() - projection.x(), p.y() - projection.y());
    };

    QList<qreal> result;
    result.reserve(path.size());
    for (const QPointF &p : path) {
        qreal best = std::numeric_limits<qreal>::max();
        for (int i = 0; i + 1 < reference.size(); ++i)
            best = qMin(best, distanceToSegment(p, reference[i], reference[i + 1]));
        result.append(best);
    }
    return result;
}

} // namespace detail

// Максимальное расстояние от точек пути до ломаной-эталона.
inline qreal maxDeviation(const QList<QPointF> &path, const QList<QPointF> &reference)
{
    qreal worst = 0.0;
    for (qreal d : detail::deviations(path, reference))
        worst = qMax(worst, d);
    return worst;
}

// Среднеквадратичное отклонение - им и меряется дрожание: одиночный выброс
// его не определяет, в отличие от максимума.
inline qreal rmsDeviation(const QList<QPointF> &path, const QList<QPointF> &reference)
{
    const QList<qreal> values = detail::deviations(path, reference);
    if (values.isEmpty())
        return 0.0;
    qreal sum = 0.0;
    for (qreal d : values)
        sum += d * d;
    return std::sqrt(sum / values.size());
}

} // namespace reference
