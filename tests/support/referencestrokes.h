#pragma once

#include <QList>
#include <QPointF>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <limits>

#include "ink/inktypes.h"
#include "ink/wetstroke.h"

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

// --- rnote-grade-stroke-fidelity (design.md, D6) -----------------------------
// Эталоны того, что ломалось: редкие отсчёты на быстром письме, острые углы,
// деталь короче кадра. Всё в экранных единицах.

// Плотная окружность: геометрия, по которой потом «ведут перо».
inline QList<QPointF> circle(qreal radius = 30.0, int loops = 2,
                             const QPointF &center = QPointF(150.0, 150.0), int count = 720)
{
    QList<QPointF> points;
    for (int i = 0; i < count; ++i) {
        const qreal t = 2.0 * M_PI * loops * i / (count - 1);
        points.append(center + QPointF(radius * std::cos(t), radius * std::sin(t)));
    }
    return points;
}

inline QList<QPointF> scaled(const QList<QPointF> &path, qreal factor)
{
    QList<QPointF> result;
    result.reserve(path.size());
    for (const QPointF &p : path)
        result.append(p * factor);
    return result;
}

// «V» и «N»: ломаные с разворотом на месте. Вершины - там, где перо
// разворачивается.
inline QList<QPointF> cornerV()
{
    return {QPointF(40, 40), QPointF(80, 120), QPointF(120, 40)};
}

inline QList<QPointF> cornerN()
{
    return {QPointF(40, 120), QPointF(40, 40), QPointF(100, 120), QPointF(100, 40)};
}

// Ведёт перо по траектории с постоянной скоростью (экранных единиц в секунду)
// и снимает отсчёты с частотой дигитайзера hz. dwellSeconds - сколько перо
// стоит в каждой внутренней вершине: так рука проходит разворот. Для плотных
// кривых стоянка должна быть нулевой. rampSeconds - за сколько перо разгоняется
// из покоя до speed (с постоянным ускорением); ноль - мгновенный старт.
inline QList<InkPoint> sampledAtSpeed(const QList<QPointF> &path, qreal speed, qreal hz,
                                      qreal dwellSeconds = 0.0, qreal pressure = 0.6,
                                      qreal rampSeconds = 0.0)
{
    struct Piece
    {
        QPointF from;
        QPointF to;
        qreal duration = 0.0;
    };
    QList<Piece> pieces;
    qreal total = 0.0;
    for (int i = 0; i + 1 < path.size(); ++i) {
        const QPointF delta = path[i + 1] - path[i];
        pieces.append({path[i], path[i + 1], std::hypot(delta.x(), delta.y()) / speed});
        total += pieces.constLast().duration;
        if (dwellSeconds > 0.0 && i + 2 < path.size()) {
            pieces.append({path[i + 1], path[i + 1], dwellSeconds});
            total += dwellSeconds;
        }
    }

    QList<InkPoint> points;
    int piece = 0;
    qreal pieceStart = 0.0;
    for (int k = 0; !pieces.isEmpty(); ++k) {
        const qreal real = k / hz;
        // Время по расписанию постоянной скорости: на разгоне перо отстаёт от
        // него на v·t²/2T, после разгона - на v·T/2.
        const qreal scheduled = rampSeconds > 0.0 && real < rampSeconds
            ? real * real / (2.0 * rampSeconds)
            : real - rampSeconds / 2.0;
        const qreal t = qMin(scheduled, total);
        while (piece + 1 < pieces.size() && t > pieceStart + pieces[piece].duration) {
            pieceStart += pieces[piece].duration;
            ++piece;
        }
        const Piece &current = pieces[piece];
        const qreal u = current.duration > 0.0
            ? qBound(0.0, (t - pieceStart) / current.duration, 1.0)
            : 1.0;
        points.append({current.from + (current.to - current.from) * u, pressure, real});
        if (scheduled >= total)
            break;
    }
    return points;
}

// Штрих, каким его сохраняет приложение: сырые отсчёты пера через WetStroke
// (StrokeSampler, точка касания D5) при масштабе zoom. Сравнивать полосу и
// контур имеет смысл на нём, а не на редкой ломаной: острые стыки редкой
// ломаной полоса на внутренней стороне покрывает меньше объединения дисков.
inline Stroke written(const QList<InkPoint> &input, const Stroke &prototype, qreal zoom = 1.0)
{
    WetStroke wet;
    wet.begin(prototype.page, prototype, zoom);
    for (const InkPoint &point : input)
        wet.addPoint(point);
    return wet.finish();
}

// Прямая с зубцом, который перо проходит быстрее одного кадра экрана в 120 Гц:
// отсчёт вверх на amplitude и следующий - обратно на линию. Всё остальное -
// ровное ведение со скоростью speed.
struct ToothStroke
{
    QList<InkPoint> points;
    QPointF apex;
    qreal amplitude = 0.0;
};

inline ToothStroke toothShorterThanFrame(qreal amplitude = 3.0, qreal speed = 300.0,
                                         qreal hz = 240.0)
{
    ToothStroke result;
    result.amplitude = amplitude;
    const int count = int(hz * 0.5);
    const int apexIndex = count / 2;
    for (int i = 0; i < count; ++i) {
        const qreal t = i / hz;
        QPointF position(40.0 + speed * t, 100.0);
        if (i == apexIndex) {
            position.ry() -= amplitude;
            result.apex = position;
        }
        result.points.append({position, 0.6, t});
    }
    return result;
}

// Метрика граней: наибольший поворот между соседними отрезками пути, в
// градусах. На кривой, нарезанной с шагом s, поворот на шаг равен s / R; грань
// между редкими отсчётами даёт скачок в десятки градусов. Концы пропускаются:
// там траектория законно начинается и кончается.
inline qreal maxTurnDegrees(const QList<QPointF> &path, int skipAtEnds = 0)
{
    qreal worst = 0.0;
    for (int i = qMax(1, skipAtEnds); i + 1 < path.size() - skipAtEnds; ++i) {
        const QPointF a = path[i] - path[i - 1];
        const QPointF b = path[i + 1] - path[i];
        if (std::hypot(a.x(), a.y()) < 1e-9 || std::hypot(b.x(), b.y()) < 1e-9)
            continue;
        const qreal turn = std::atan2(a.x() * b.y() - a.y() * b.x(), QPointF::dotProduct(a, b));
        worst = qMax(worst, std::abs(turn) * 180.0 / M_PI);
    }
    return worst;
}

// Выпуклая оболочка (монотонная цепь), против часовой стрелки.
inline QList<QPointF> convexHull(QList<QPointF> points)
{
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    const auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    QList<QPointF> hull;
    for (int pass = 0; pass < 2; ++pass) {
        const qsizetype start = hull.size();
        for (qsizetype i = 0; i < points.size(); ++i) {
            const QPointF &p = pass == 0 ? points[i] : points[points.size() - 1 - i];
            while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.last(), p) <= 0)
                hull.removeLast();
            hull.append(p);
        }
        hull.removeLast();
    }
    return hull;
}

// Насколько точка вышла за выпуклую оболочку; 0 - внутри или на границе.
inline qreal distanceOutsideHull(const QPointF &p, const QList<QPointF> &hull)
{
    bool inside = true;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (qsizetype i = 0; i < hull.size(); ++i) {
        const QPointF &a = hull[i];
        const QPointF &b = hull[(i + 1) % hull.size()];
        const QPointF ab = b - a;
        if (ab.x() * (p.y() - a.y()) - ab.y() * (p.x() - a.x()) < 0)
            inside = false;
        const qreal lengthSquared = QPointF::dotProduct(ab, ab);
        const qreal t = lengthSquared < 1e-12
            ? 0.0
            : qBound(0.0, QPointF::dotProduct(p - a, ab) / lengthSquared, 1.0);
        const QPointF projection = a + ab * t;
        nearest = qMin(nearest, std::hypot(p.x() - projection.x(), p.y() - projection.y()));
    }
    return inside ? 0.0 : nearest;
}

// Ближайшее расстояние от точки до пути.
inline qreal distanceToPath(const QPointF &p, const QList<QPointF> &path)
{
    qreal best = std::numeric_limits<qreal>::max();
    for (const QPointF &q : path)
        best = qMin(best, std::hypot(p.x() - q.x(), p.y() - q.y()));
    return best;
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
