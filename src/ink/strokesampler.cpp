#include "strokesampler.h"

#include <QtGlobal>

#include <cmath>

#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/types.h"

namespace {

namespace ism = ink::stroke_model;

// Параметры подобраны перебором по эталонам tst_strokesampler сразу (задача
// 4.6). Стартовой точкой был набор rnote (сборщик Modeled): его координаты
// документа на 100% совпадают с логическими пикселями, а модель у нас работает
// в экранных логических пикселях. На нём проваливались пять эталонов: размах
// быстрого зигзага 67%, радиус быстрой окружности уходил на 6.6 px, форма
// быстрой буквы - на 3.8 px, зубец короче кадра - 0.33 из 3, грани до 9°.

// Пружина: собственная частота 67 рад/с (10.7 Гц) при затухании 0.7. Быстрое
// движение ведёт следование за пером (ниже), пружине остаётся медленное. Жёстче -
// хуже гасится дрожь: при 80 рад/с без подавления дрожи остаётся 48% от исходной
// (smoothsJitterOnSlowStroke требует меньше 45%). Мягче, 54 рад/с как у rnote, -
// сильнее сжимаются мелкие петли (smallLoopsKeepTheirSize: 81% радиуса против
// 92%).
constexpr float kSpringMassConstant = 2.2e-4f; // 1 / 67²
constexpr float kDragConstant = 94.0f;         // 2 · 0.7 · 67

// Следование за пером (FollowState). Встроенное в библиотеку подавление сжатия
// петель смешивает пружину с ломаной отсчётов по линейной функции скорости, и
// на эталонах оно не выдерживает двух порогов сразу: с узкой полосой скоростей
// на разгоне пера появляются изломы 8-9° (fastCircleHasNoFacets), с широкой -
// зубец в один отсчёт гасится до 0.73 из 3 (toothShorterThanFrameSurvives).
// Здесь доля следования - smoothstep от сглаженной скорости модели.
//
// Полоса 20..200 px/с. Первая версия, 50..400, подбиралась только на крупных
// эталонах, и при сравнении с rnote на живом письме мелкие «о» сжимались в «а» и
// точки: петлю в 8-20 px рука ведёт на 100-400 px/с, и доля следования там была
// мала (smallLoopsKeepTheirSize: радиус 61% против 92% сейчас). Дрожь на 60 px/с
// получает долю около 0.13 и остаётся под пружиной. Итог на эталонах: грань
// быстрой окружности 3.7°, её радиус 0.1 px, зубец 2.96 из 3, дрожь 0.41,
// размах зигзага 0.99. Сглаживание скорости 10 мс: при 20 мс (на полосе
// 50..400) радиус быстрой окружности уходил на 0.7 px вместо 0.1.
constexpr qreal kFollowSpeedLower = 20.0;
constexpr qreal kFollowSpeedUpper = 200.0;
constexpr qreal kFollowSpeedTimeConstant = 0.01;
// Положение пера между отсчётами - по сплайну, а не по хорде. Нужно, когда
// модель досэмплирует, то есть перо сообщает положение реже kMinOutputRate:
// узел на хорде лёг бы внутрь дуги, и сплайн через него ломался бы в каждом
// отсчёте (при 120 Гц на 120 Гц - 10.2° по хорде против 6.6° по сплайну).
constexpr bool kFollowAlongSpline = true;
constexpr int kRawHistory = 64;

// Подавление дрожи - как у rnote: пороги 1.31 и 1.44 px/с, то есть фактически
// выключено. Полное сглаживание ниже 150 px/с стягивало мелкие петли: усреднение
// положения за 40 мс на петле в 8-20 px - заметная доля её оборота
// (smallLoopsKeepTheirSize: 71% радиуса без него против 61% с ним при прежней
// пружине). Дрожь гасит пружина.
constexpr double kWobbleTimeout = 0.04;
constexpr float kWobbleSpeedFloor = 1.31f;
constexpr float kWobbleSpeedCeiling = 1.44f;

// Частота выхода ниже, чем у rnote (120 Гц), и ниже частоты пера и мыши (от
// 120 Гц): тогда модель выдаёт ровно один узел на отсчёт. При 120 Гц на отсчётах
// в 120 Гц ошибка округления давала два шага на интервал, и промежуточный узел
// ломал сплайн в каждом отсчёте (fastCircleHasNoFacets: 6.6-10.2° против 3.7°).
constexpr double kMinOutputRate = 100.0;
constexpr float kEndOfStrokeStoppingDistance = 0.01f;
constexpr int kEndOfStrokeMaxIterations = 20;
// У rnote здесь 200, то есть модель отказывается от отсчёта, пришедшего позже
// чем через 1.7 с после предыдущего. Перо, замершее на странице, так и делает:
// событий нет, пока оно не сдвинется. Отказ означал бы потерянный отсчёт,
// поэтому потолок - значение библиотеки по умолчанию.
constexpr int kMaxOutputsPerCall = 100000;

// Давление сглаживается отдельно от траектории: скачок давления виден
// ступенькой толщины. Срез 12 Гц (τ ≈ 13 мс) вместо прежних 2.5 Гц (τ ≈ 64 мс):
// с прежним начало штриха оставалось бледным на 36 px, а конец не успевал
// сойти по спаду нажима (ink-beauty-pass, design.md, D2). beta 0.5 поднимает
// срез на быстром нажиме: на заходе 0.05 -> 0.65 за 80 мс он около 15 Гц. Шум
// давления при этом проходит честно; если на устройстве при ровном нажиме
// видна рябь толщины, minCutoff снижается до 8 Гц - это одна константа.
constexpr qreal kPressureMinCutoff = 12.0;
constexpr qreal kPressureBeta = 0.5;
constexpr qreal kPressureDerivativeCutoff = 10.0;

// Шаг прохода по сплайну в долях шага нарезки: точки нарезки ложатся на
// хорды этих мелких шагов, и прогиб дуги на них уже не виден.
constexpr qreal kFlattenFraction = 0.5;

// Ближе этого (в координатах страницы) конец траектории считается уже лежащим
// в точке отрыва.
constexpr qreal kEndSnapDistance = 1e-4;

ism::StrokeModelParams modelParams()
{
    ism::StrokeModelParams params;

    params.wobble_smoother_params.is_enabled = true;
    params.wobble_smoother_params.timeout = ism::Duration(kWobbleTimeout);
    params.wobble_smoother_params.speed_floor = kWobbleSpeedFloor;
    params.wobble_smoother_params.speed_ceiling = kWobbleSpeedCeiling;

    params.position_modeler_params.spring_mass_constant = kSpringMassConstant;
    params.position_modeler_params.drag_constant = kDragConstant;

    params.sampling_params.min_output_rate = kMinOutputRate;
    params.sampling_params.end_of_stroke_stopping_distance = kEndOfStrokeStoppingDistance;
    params.sampling_params.end_of_stroke_max_iterations = kEndOfStrokeMaxIterations;
    params.sampling_params.max_outputs_per_call = kMaxOutputsPerCall;

    // Предсказатель догоняет модель до последнего положения пера и не заходит
    // за него: хвост не торчит в прежнем направлении на развороте (D5).
    params.prediction_params = ism::StrokeEndPredictorParams{};
    return params;
}

qreal distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

// Centripetal Catmull-Rom (α = 0.5), алгоритм Барри-Голдмана, на сегменте
// p1 -> p2. Centripetal - единственный из вариантов без петель и выбросов
// внутри сегмента: не рисует петель, которых не было, и не выносит угол наружу.
QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                   qreal u)
{
    // Совпадающие соседние узлы дали бы нулевой интервал параметра.
    const auto knot = [](const QPointF &a, const QPointF &b) {
        return std::sqrt(qMax(distance(a, b), 1e-6));
    };
    const qreal t0 = 0.0;
    const qreal t1 = t0 + knot(p0, p1);
    const qreal t2 = t1 + knot(p1, p2);
    const qreal t3 = t2 + knot(p2, p3);
    const qreal t = t1 + (t2 - t1) * u;

    const QPointF a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const QPointF a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const QPointF a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const QPointF b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const QPointF b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

qreal lerpPressure(const InkPoint &from, const InkPoint &to, qreal u)
{
    if (from.hasPressure() && to.hasPressure())
        return from.pressure + (to.pressure - from.pressure) * u;
    return to.pressure;
}

} // namespace

StrokeSampler::StrokeSampler()
    : m_modeler(std::make_unique<ism::StrokeModeler>())
    , m_pressureFilter(makePressureFilter())
{
    [[maybe_unused]] const absl::Status status = m_modeler->Reset(modelParams());
    Q_ASSERT_X(status.ok(), "StrokeSampler", "параметры модели штриха не прошли проверку");
}

StrokeSampler::~StrokeSampler() = default;

OneEuroFilter StrokeSampler::makePressureFilter()
{
    return OneEuroFilter(kPressureMinCutoff, kPressureBeta, kPressureDerivativeCutoff);
}

void StrokeSampler::setSpacing(qreal spacing)
{
    m_screenSpacing = qMax(spacing, 0.05);
}

void StrokeSampler::setScale(qreal scale)
{
    m_scale = qBound(0.01, scale, 100.0);
}

void StrokeSampler::reset()
{
    [[maybe_unused]] const absl::Status status = m_modeler->Reset();
    m_pressureFilter.reset();
    m_started = false;
    m_finished = false;
    m_timeOrigin = 0.0;
    m_lastInputTime = 0.0;
    m_lastInputScreen = QPointF();
    m_lastInputPressure = -1.0;
    m_lastRaw = InkPoint();
    m_follow = FollowState();
    m_rawHistory.clear();
    m_nodes.clear();
    m_nextSegment = 0;
    m_points.clear();
    m_provisional.clear();
}

QList<InkPoint> StrokeSampler::append(const InkPoint &raw)
{
    if (m_finished)
        return {};

    const QPointF screen = raw.position * m_scale;
    qreal time = raw.timeSeconds;
    if (m_started) {
        // Модель требует неубывающего времени (D2).
        time = qMax(time - m_timeOrigin, m_lastInputTime);
        if (time == m_lastInputTime && screen == m_lastInputScreen)
            return {};
    } else {
        m_timeOrigin = time;
        time = 0.0;
    }

    // Отсутствие давления пробрасывается как есть: сглаживать нечего, а -1
    // должно дойти до тесселятора нетронутым.
    const qreal pressure = raw.hasPressure()
        ? qBound(0.0, m_pressureFilter.filter(raw.pressure, time), 1.0)
        : -1.0;

    ism::Input input;
    input.event_type = m_started ? ism::Input::EventType::kMove : ism::Input::EventType::kDown;
    input.position = {float(screen.x()), float(screen.y())};
    input.time = ism::Time(time);
    input.pressure = float(pressure);

    m_results.clear();
    if (!m_modeler->Update(input, m_results).ok())
        return {}; // отсчёт, который модель не приняла, не ломает штрих

    m_started = true;
    m_lastInputTime = time;
    m_lastInputScreen = screen;
    m_lastInputPressure = pressure;
    m_lastRaw = raw;
    m_rawHistory.append({screen, pressure, time});
    while (m_rawHistory.size() > kRawHistory)
        m_rawHistory.removeFirst();

    for (const ism::Result &result : m_results)
        appendNode(result);

    const QList<InkPoint> added = emitSettled(false);
    rebuildProvisional();
    return added;
}

QList<InkPoint> StrokeSampler::finish()
{
    if (m_finished || !m_started)
        return {};
    m_finished = true;
    m_provisional.clear();

    // Отрыв: модель догоняет последнее положение пера.
    ism::Input up;
    up.event_type = ism::Input::EventType::kUp;
    up.position = {float(m_lastInputScreen.x()), float(m_lastInputScreen.y())};
    up.time = ism::Time(m_lastInputTime);
    up.pressure = float(m_lastInputPressure);
    m_results.clear();
    if (m_modeler->Update(up, m_results).ok()) {
        for (const ism::Result &result : m_results)
            appendNode(result);
    }

    QList<InkPoint> added = emitSettled(true);

    // Хвост короче шага нарезки остался бы отброшенным, а штрих обязан
    // кончаться в последней точке контакта - сценарий «отрыв пера». emitSettled
    // уже положил свои точки в m_points, здесь - только дотяжка. Порог - не
    // ноль: выход модели во float, и точка, совпавшая с контактом, отстоит от
    // него на ошибку округления. Без порога касание без движения давало бы
    // штрих из двух точек в одном месте.
    //
    // Положение конца - сырое, а давление - отфильтрованное, как у всех точек
    // штриха: сырое давление последнего отсчёта на спаде нажима меньше
    // отфильтрованного, и толщина на последней точке скакала (на стенде - на
    // 56%), оставляя «клюв» (ink-beauty-pass, design.md, D2).
    InkPoint end = m_lastRaw;
    if (end.hasPressure())
        end.pressure = m_lastInputPressure;
    InkPoint last = m_points.constLast();
    QList<InkPoint> tail;
    walkTowards(last, end, tail);
    if (distance(last.position, end.position) > kEndSnapDistance)
        tail.append(end);

    m_points.append(tail);
    added.append(tail);
    return added;
}

QPointF StrokeSampler::rawAt(qreal time) const
{
    if (m_rawHistory.isEmpty())
        return {};
    if (time >= m_rawHistory.constLast().timeSeconds)
        return m_rawHistory.constLast().position;
    for (qsizetype i = m_rawHistory.size() - 1; i > 0; --i) {
        const InkPoint &a = m_rawHistory[i - 1];
        const InkPoint &b = m_rawHistory[i];
        if (time >= a.timeSeconds) {
            const qreal span = b.timeSeconds - a.timeSeconds;
            const qreal u = span > 0.0 ? (time - a.timeSeconds) / span : 1.0;
            if (!kFollowAlongSpline)
                return a.position + (b.position - a.position) * u;
            // Между отсчётами перо шло не по хорде: узел модели, пришедшийся
            // между ними, лёг бы внутрь дуги, и сплайн через такие узлы
            // ломался бы в каждом отсчёте. Следующего отсчёта у последнего
            // интервала ещё нет - сосед отражается.
            const QPointF p0 = i >= 2 ? m_rawHistory[i - 2].position : a.position * 2.0 - b.position;
            const QPointF p3 =
                i + 1 < m_rawHistory.size() ? m_rawHistory[i + 1].position : b.position * 2.0 - a.position;
            return catmullRom(p0, a.position, b.position, p3, u);
        }
    }
    return m_rawHistory.constFirst().position;
}

qreal StrokeSampler::pressureAt(qreal time) const
{
    if (m_rawHistory.isEmpty())
        return -1.0;
    if (time >= m_rawHistory.constLast().timeSeconds)
        return m_rawHistory.constLast().pressure;
    for (qsizetype i = m_rawHistory.size() - 1; i > 0; --i) {
        const InkPoint &a = m_rawHistory[i - 1];
        const InkPoint &b = m_rawHistory[i];
        if (time >= a.timeSeconds) {
            // Отсчёт без давления (-1) с настоящим давлением не смешивается,
            // как в lerpPressure: смесь дала бы отрицательное давление или
            // тонкое место.
            if (!a.hasPressure() || !b.hasPressure())
                return b.pressure;
            const qreal span = b.timeSeconds - a.timeSeconds;
            const qreal u = span > 0.0 ? (time - a.timeSeconds) / span : 1.0;
            return a.pressure + (b.pressure - a.pressure) * u;
        }
    }
    return m_rawHistory.constFirst().pressure;
}

InkPoint StrokeSampler::followedNode(const ism::Result &result, FollowState &state) const
{
    const qreal time = result.time.Value();
    const qreal speed = std::hypot(result.velocity.x, result.velocity.y);
    if (state.primed) {
        const qreal dt = qMax(0.0, time - state.time);
        state.speed += (1.0 - std::exp(-dt / kFollowSpeedTimeConstant)) * (speed - state.speed);
    } else {
        state.speed = speed;
        state.primed = true;
    }
    state.time = time;

    const qreal x = qBound(
        0.0, (state.speed - kFollowSpeedLower) / (kFollowSpeedUpper - kFollowSpeedLower), 1.0);
    const qreal share = x * x * (3.0 - 2.0 * x);

    const QPointF modeled(result.position.x, result.position.y);
    InkPoint node;
    node.position = modeled + (rawAt(time) - modeled) * share;
    // Давление - пера в момент узла, а не проекция пружины (см. pressureAt).
    // Догон на отрыве и предсказание уходят за последний отсчёт и получают
    // его давление.
    node.pressure = m_lastInputPressure < 0.0 ? -1.0 : pressureAt(time);
    // Догон на отрыве идёт шагами модели дальше времени последнего отсчёта.
    // Время точки обязано быть временем пера, а не модели.
    node.timeSeconds = qMin(time, m_lastInputTime);
    return node;
}

void StrokeSampler::appendNode(const ism::Result &result)
{
    const InkPoint node = followedNode(result, m_follow);

    // Перо стоит на месте: узел в той же точке сплайну ничего не даёт, только
    // вырожденный сегмент. Давление и время у него при этом обновляются.
    if (!m_nodes.isEmpty() && distance(m_nodes.constLast().position, node.position) < 1e-3
        && m_nextSegment + 1 < m_nodes.size()) {
        m_nodes.last().pressure = node.pressure;
        m_nodes.last().timeSeconds = node.timeSeconds;
        return;
    }
    m_nodes.append(node);
}

InkPoint StrokeSampler::toPage(const InkPoint &node) const
{
    return {node.position / m_scale, node.pressure, node.timeSeconds + m_timeOrigin};
}

template <typename Sink>
void StrokeSampler::flattenSegment(const QList<InkPoint> &nodes, int k, Sink sink) const
{
    const InkPoint &n1 = nodes[k];
    const InkPoint &n2 = nodes[k + 1];
    const QPointF p1 = n1.position;
    const QPointF p2 = n2.position;
    const QPointF p0 = k > 0 ? nodes[k - 1].position : p1 * 2.0 - p2;
    const QPointF p3 = k + 2 < nodes.size() ? nodes[k + 2].position : p2 * 2.0 - p1;

    const int steps = qBound(
        1, int(std::ceil(distance(p1, p2) / (m_screenSpacing * kFlattenFraction))), 4096);
    for (int i = 1; i <= steps; ++i) {
        const qreal u = qreal(i) / steps;
        InkPoint dense;
        dense.position = i == steps ? p2 : catmullRom(p0, p1, p2, p3, u);
        dense.pressure = lerpPressure(n1, n2, u);
        dense.timeSeconds = n1.timeSeconds + (n2.timeSeconds - n1.timeSeconds) * u;
        sink(toPage(dense));
    }
}

void StrokeSampler::walkTowards(InkPoint &last, const InkPoint &target, QList<InkPoint> &out) const
{
    const qreal step = spacing();
    while (true) {
        const QPointF delta = target.position - last.position;
        const qreal length = std::hypot(delta.x(), delta.y());
        if (length < step)
            break;

        const qreal t = step / length;
        InkPoint next;
        next.position = last.position + delta * t;
        next.timeSeconds = last.timeSeconds + (target.timeSeconds - last.timeSeconds) * t;
        next.pressure = lerpPressure(last, target, t);

        out.append(next);
        last = next;
    }
}

// Нарезает окончательные точки по сегментам, у которых уже есть следующий
// сосед. С toEnd - и по последнему сегменту тоже, с отражённым соседом.
QList<InkPoint> StrokeSampler::emitSettled(bool toEnd)
{
    QList<InkPoint> added;
    if (m_nodes.isEmpty())
        return added;

    if (m_points.isEmpty()) {
        const InkPoint first = toPage(m_nodes.constFirst());
        m_points.append(first);
        added.append(first);
    }

    InkPoint last = m_points.constLast();
    const int firstNew = int(added.size());
    while (m_nextSegment + 1 < m_nodes.size() && (toEnd || m_nextSegment + 2 < m_nodes.size())) {
        flattenSegment(m_nodes, m_nextSegment,
                       [&](const InkPoint &dense) { walkTowards(last, dense, added); });
        ++m_nextSegment;
    }
    m_points.append(added.mid(firstNew));

    // Сплайну нужен один узел до текущего сегмента, остальные уже не нужны.
    const int drop = m_nextSegment - 1;
    if (drop > 0) {
        m_nodes.remove(0, drop);
        m_nextSegment -= drop;
    }
    return added;
}

// Провизорный хвост: остаток сплайна без следующего соседа и предсказание
// модели до последнего положения пера. Работа ограничена длиной хвоста и не
// зависит от длины штриха.
void StrokeSampler::rebuildProvisional()
{
    m_provisional.clear();
    if (m_points.isEmpty())
        return;

    QList<InkPoint> tail = m_nodes.mid(qMax(0, m_nextSegment - 1));
    int first = m_nextSegment > 0 ? 1 : 0;

    m_prediction.clear();
    if (m_modeler->Predict(m_prediction).ok()) {
        // Состояние следования копируется: предсказание не должно его сдвигать.
        FollowState follow = m_follow;
        for (const ism::Result &result : m_prediction) {
            const InkPoint node = followedNode(result, follow);
            if (!tail.isEmpty() && distance(tail.constLast().position, node.position) < 1e-3)
                continue;
            tail.append(node);
        }
    }

    InkPoint last = m_points.constLast();
    for (int k = first; k + 1 < tail.size(); ++k)
        flattenSegment(tail, k, [&](const InkPoint &dense) { walkTowards(last, dense, m_provisional); });

    if (tail.size() > first) {
        const InkPoint end = toPage(tail.constLast());
        if (distance(last.position, end.position) > 1e-9)
            m_provisional.append(end);
    }
}
