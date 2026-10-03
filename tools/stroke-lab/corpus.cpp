#include "corpus.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "support/referencestrokes.h"

namespace lab {

bool Sample::hasTag(const std::string &tag) const
{
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

namespace {

// --- детерминированный генератор -------------------------------------------
// Свой, а не std::*_distribution: у тех реализация зависит от библиотеки.
class Rng
{
public:
    explicit Rng(uint64_t seed) : m_state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    double gauss()
    {
        const double u1 = std::max(uniform(), 1e-300);
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }

private:
    uint64_t m_state;
};

double len(const QPointF &v) { return std::hypot(v.x(), v.y()); }

double smoothstep(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// Части штриха между остриями (cusp): в острие скорость пера падает до нуля.
struct Shape
{
    std::vector<std::vector<QPointF>> parts;
};

// Centripetal Catmull-Rom - как в StrokeSampler.
QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                   double u)
{
    const auto knot = [](const QPointF &a, const QPointF &b) {
        return std::sqrt(std::max(len(b - a), 1e-6));
    };
    const double t0 = 0.0;
    const double t1 = t0 + knot(p0, p1);
    const double t2 = t1 + knot(p1, p2);
    const double t3 = t2 + knot(p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const QPointF a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const QPointF a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const QPointF a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const QPointF b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const QPointF b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

// Плотная гладкая кривая через точки части (шаг ~0.05 px).
std::vector<QPointF> densify(const std::vector<QPointF> &in)
{
    std::vector<QPointF> pts;
    for (const QPointF &p : in) {
        if (pts.empty() || len(p - pts.back()) > 1e-6)
            pts.push_back(p);
    }
    std::vector<QPointF> out;
    if (pts.size() < 2) {
        out = pts;
        return out;
    }
    out.push_back(pts.front());
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const QPointF p1 = pts[k];
        const QPointF p2 = pts[k + 1];
        const QPointF p0 = k > 0 ? pts[k - 1] : p1 * 2.0 - p2;
        const QPointF p3 = k + 2 < pts.size() ? pts[k + 2] : p2 * 2.0 - p1;
        const int steps = std::max(1, int(std::ceil(len(p2 - p1) / 0.05)));
        for (int i = 1; i <= steps; ++i) {
            const double u = double(i) / steps;
            out.push_back(i == steps ? p2 : catmullRom(p0, p1, p2, p3, u));
        }
    }
    return out;
}

struct Motion
{
    double cruise = 300.0;   // скорость на кривизне 1/curvatureRadius, px/s
    double curvatureRadius = 10.0;
    double rampIn = 0.035;   // разгон из острия / касания, с
    double rampOut = 0.045;  // торможение к острию / отрыву, с
    double cuspDwell = 0.006;
    double tremor = 0.12;    // амплитуда физиологического тремора, px
    uint64_t seed = 1;
};

// Время по траектории: закон 2/3 (v ~ k^(-1/3)) плюс разгон и торможение до
// нуля в каждом острие - скорость руки в острие падает до нуля.
TruePath timeParameterize(const Shape &shape, const Motion &motion, double startTime,
                          Rng &rng)
{
    TruePath path;
    constexpr double dt = 1e-4;
    double t = startTime;
    const double pressurePhase1 = rng.uniform(0, 2 * M_PI);
    const double pressurePhase2 = rng.uniform(0, 2 * M_PI);
    const double basePressure = rng.uniform(0.48, 0.6);

    std::vector<double> speeds;
    for (size_t partIndex = 0; partIndex < shape.parts.size(); ++partIndex) {
        const std::vector<QPointF> dense = densify(shape.parts[partIndex]);
        if (dense.size() < 2)
            continue;
        std::vector<double> arc(dense.size(), 0.0);
        for (size_t i = 1; i < dense.size(); ++i)
            arc[i] = arc[i - 1] + len(dense[i] - dense[i - 1]);
        const double total = arc.back();

        // Кривизна по окну ±1 px.
        std::vector<double> curvature(dense.size(), 0.0);
        {
            size_t lo = 0, hi = 0;
            for (size_t i = 0; i < dense.size(); ++i) {
                while (lo < i && arc[i] - arc[lo] > 1.0)
                    ++lo;
                while (hi + 1 < dense.size() && arc[hi] - arc[i] < 1.0)
                    ++hi;
                if (lo == i || hi == i)
                    continue;
                const QPointF a = dense[i] - dense[lo];
                const QPointF b = dense[hi] - dense[i];
                const double turn = std::atan2(a.x() * b.y() - a.y() * b.x(),
                                               a.x() * b.x() + a.y() * b.y());
                curvature[i] = std::abs(turn) / std::max(arc[hi] - arc[lo], 1e-6) * 2.0;
            }
        }
        const double kref = 1.0 / motion.curvatureRadius;
        const auto plSpeed = [&](size_t i) {
            const double factor = std::cbrt(kref / (curvature[i] + 1e-4));
            return motion.cruise * std::clamp(factor, 0.35, 1.6);
        };

        const double decel = std::min(motion.cruise * motion.rampOut / 2.0, total / 2.0);
        const double rampIn = std::min(motion.rampIn, 2.0 * total / motion.cruise + 0.004);
        double s = 0.0;
        size_t idx = 0;
        double tau = 0.0;
        while (true) {
            while (idx + 1 < dense.size() && arc[idx + 1] < s)
                ++idx;
            const size_t j = std::min(idx + 1, dense.size() - 1);
            const double span = arc[j] - arc[idx];
            const double u = span > 0 ? std::clamp((s - arc[idx]) / span, 0.0, 1.0) : 0.0;
            const QPointF pos = dense[idx] + (dense[j] - dense[idx]) * u;
            const double vpl = plSpeed(idx);
            const double rem = total - s;
            const double ein = 0.03 + 0.97 * smoothstep(tau / rampIn);
            const double eout = std::sqrt(std::clamp(rem / std::max(decel, 1e-6), 0.0, 1.0));
            double v = std::max(vpl * std::min(ein, eout), 1.5);

            path.t.push_back(t);
            path.p.push_back(pos);
            path.speed.push_back(v);
            if (rem <= 1e-3)
                break;
            s = std::min(total, s + v * dt);
            t += dt;
            tau += dt;
        }
        path.speed.back() = 0.0;
        // Стоянка в острие (кроме конца штриха).
        if (partIndex + 1 < shape.parts.size()) {
            const QPointF pos = path.p.back();
            for (double w = dt; w < motion.cuspDwell; w += dt) {
                t += dt;
                path.t.push_back(t);
                path.p.push_back(pos);
                path.speed.push_back(0.0);
            }
        }
        t += dt;
    }

    // Давление: разгон нажима на касании, сброс на отрыве, медленная живая
    // составляющая и чуть легче на быстром.
    const double t0 = path.t.empty() ? 0.0 : path.t.front();
    const double t1 = path.t.empty() ? 0.0 : path.t.back();
    path.pressure.resize(path.t.size());
    for (size_t i = 0; i < path.t.size(); ++i) {
        const double local = path.t[i] - t0;
        const double base = basePressure + 0.07 * std::sin(2 * M_PI * 0.9 * local + pressurePhase1)
            + 0.035 * std::sin(2 * M_PI * 2.3 * local + pressurePhase2)
            - 0.1 * std::clamp(path.speed[i] / 1500.0, 0.0, 1.0);
        const double rin = smoothstep(local / 0.03);
        const double rout = smoothstep((t1 - path.t[i]) / 0.04);
        path.pressure[i] = std::clamp(0.05 + (base - 0.05) * rin * rout, 0.0, 1.0);
    }
    return path;
}

QPointF truthAt(const TruePath &path, double t)
{
    if (t <= path.t.front())
        return path.p.front();
    if (t >= path.t.back())
        return path.p.back();
    const auto it = std::upper_bound(path.t.begin(), path.t.end(), t);
    const size_t j = size_t(it - path.t.begin());
    const size_t i = j - 1;
    const double u = (t - path.t[i]) / std::max(path.t[j] - path.t[i], 1e-12);
    return path.p[i] + (path.p[j] - path.p[i]) * u;
}

double pressureAt(const TruePath &path, double t)
{
    if (t <= path.t.front())
        return path.pressure.front();
    if (t >= path.t.back())
        return path.pressure.back();
    const auto it = std::upper_bound(path.t.begin(), path.t.end(), t);
    const size_t j = size_t(it - path.t.begin());
    const size_t i = j - 1;
    const double u = (t - path.t[i]) / std::max(path.t[j] - path.t[i], 1e-12);
    return path.pressure[i] + (path.pressure[j] - path.pressure[i]) * u;
}

struct Tremor
{
    double fx[3], fy[3], px[3], py[3], ax[3], ay[3], mod;
    QPointF at(double t) const
    {
        double x = 0, y = 0;
        for (int j = 0; j < 3; ++j) {
            x += ax[j] * std::sin(2 * M_PI * fx[j] * t + px[j]);
            y += ay[j] * std::sin(2 * M_PI * fy[j] * t + py[j]);
        }
        const double m = 1.0 + 0.3 * std::sin(2 * M_PI * 1.3 * t + mod);
        return QPointF(x, y) * m;
    }
};

Tremor makeTremor(double amplitude, Rng &rng)
{
    Tremor tr;
    for (int j = 0; j < 3; ++j) {
        tr.fx[j] = rng.uniform(8.0, 12.0);
        tr.fy[j] = rng.uniform(8.0, 12.0);
        tr.px[j] = rng.uniform(0, 2 * M_PI);
        tr.py[j] = rng.uniform(0, 2 * M_PI);
        tr.ax[j] = amplitude / 2.0 * rng.uniform(0.7, 1.3);
        tr.ay[j] = amplitude / 2.0 * rng.uniform(0.7, 1.3);
    }
    tr.mod = rng.uniform(0, 2 * M_PI);
    return tr;
}

// Отсчёты пера: 481 Гц, метка времени округлена вниз до 1 мс, положение -
// эталон плюс тремор плюс шум датчика, квантование wl_fixed (1/256 px),
// давление - 4096 уровней.
void sampleEvents(SampleStroke &stroke, const Motion &motion, const CorpusOptions &options,
                  double clockOrigin, Rng &rng)
{
    const TruePath &path = stroke.truth;
    const Tremor tremor = makeTremor(motion.tremor, rng);
    const double period = 1.0 / options.rateHz;
    const double phase = rng.uniform(0.0, period);
    const auto quant = [](double v, double q) { return std::round(v / q) * q; };
    std::vector<QPointF> history;
    for (int k = 0;; ++k) {
        const double t = path.t.front() + phase * (k > 0 ? 1.0 : 0.0) + k * period;
        if (t > path.t.back())
            break;
        QPointF pos = truthAt(path, t) + tremor.at(t)
            + QPointF(rng.gauss(), rng.gauss()) * options.noiseSigma;
        if (options.libinputSmoothing) {
            history.push_back(pos);
            if (history.size() > 4)
                history.erase(history.begin());
            QPointF sum;
            for (const QPointF &h : history)
                sum += h;
            pos = sum / double(history.size());
        }
        pos = QPointF(quant(pos.x(), options.positionQuantum), quant(pos.y(), options.positionQuantum));
        double pressure = pressureAt(path, t) + rng.gauss() * options.pressureNoise;
        pressure = std::clamp(quant(pressure, 1.0 / 4096.0), 0.0, 1.0);
        const double stamp =
            std::floor((clockOrigin + t) / options.timestampResolution + 1e-9)
            * options.timestampResolution;
        stroke.events.push_back({pos, pressure, stamp});
        stroke.eventTrueTime.push_back(t);
    }
}

// --- рукописные глифы --------------------------------------------------------
// Единицы - высота строчной (x-height = 1), y вверх, базовая линия 0. Части -
// отрезки между остриями. Первая точка - вход, последняя - выход.

struct Glyph
{
    std::vector<std::vector<QPointF>> parts;
    std::vector<std::vector<std::vector<QPointF>>> extras; // отдельные штрихи (точка над i)
};

using P = QPointF;

Glyph glyph(char32_t c)
{
    switch (c) {
    case U'i':
        return {{{P(0, .25), P(.18, .6), P(.34, 1.0)},
                 {P(.34, 1.0), P(.3, .45), P(.33, .1), P(.45, 0), P(.62, .1), P(.72, .25)}},
                {{{P(.40, 1.45), P(.43, 1.49)}}}};
    case U'n':
    case U'п':
        return {{{P(0, .25), P(.12, .62), P(.22, 1.0)},
                 {P(.22, 1.0), P(.2, .5), P(.18, 0)},
                 {P(.18, 0), P(.24, .55), P(.4, .95), P(.58, .95), P(.66, .6), P(.66, .15),
                  P(.78, 0), P(.92, .1), P(1.0, .25)}},
                {}};
    case U'm':
    case U'т':
        return {{{P(0, .25), P(.12, .62), P(.2, 1.0)},
                 {P(.2, 1.0), P(.19, .5), P(.18, 0)},
                 {P(.18, 0), P(.23, .55), P(.36, .95), P(.52, .95), P(.58, .6), P(.58, 0)},
                 {P(.58, 0), P(.63, .55), P(.76, .95), P(.92, .95), P(.98, .6), P(.98, .15),
                  P(1.08, 0), P(1.2, .1), P(1.28, .25)}},
                {}};
    case U'u':
    case U'и':
        return {{{P(0, .25), P(.15, .6), P(.26, 1.0)},
                 {P(.26, 1.0), P(.21, .4), P(.26, .08), P(.4, 0), P(.55, .1), P(.66, .5),
                  P(.74, 1.0)},
                 {P(.74, 1.0), P(.7, .4), P(.73, .08), P(.85, 0), P(.98, .12), P(1.05, .25)}},
                {}};
    case U'h':
        return {{{P(0, .25), P(.2, .8), P(.38, 1.6), P(.42, 2.1), P(.34, 2.3), P(.23, 2.1),
                  P(.21, 1.4), P(.19, .6), P(.18, 0)},
                 {P(.18, 0), P(.24, .55), P(.4, .95), P(.58, .95), P(.66, .6), P(.66, .15),
                  P(.78, 0), P(.92, .1), P(1.0, .25)}},
                {}};
    case U'e':
    case U'е':
        return {{{P(0, .25), P(.22, .38), P(.42, .58), P(.48, .84), P(.36, .98), P(.2, .86),
                  P(.15, .5), P(.22, .14), P(.4, 0), P(.58, .05), P(.72, .25)}},
                {}};
    case U'l':
        return {{{P(0, .25), P(.2, .8), P(.38, 1.6), P(.42, 2.1), P(.34, 2.3), P(.23, 2.1),
                  P(.21, 1.4), P(.21, .5), P(.27, .1), P(.4, 0), P(.56, .08), P(.68, .25)}},
                {}};
    case U'o':
        return {{{P(0, .25), P(.2, .6), P(.5, .98)},
                 {P(.5, .98), P(.28, .9), P(.15, .55), P(.2, .15), P(.4, 0), P(.6, .12),
                  P(.67, .5), P(.6, .88), P(.5, .98), P(.62, .95), P(.82, .9), P(.95, .88)}},
                {}};
    case U'р':
        return {{{P(0, .25), P(.12, .62), P(.2, 1.0)},
                 {P(.2, 1.0), P(.17, 0), P(.14, -.95)},
                 {P(.14, -.95), P(.16, 0), P(.22, .6), P(.42, .98), P(.62, .85), P(.66, .45),
                  P(.5, .08), P(.28, .08)},
                 {P(.28, .08), P(.55, .12), P(.8, .25)}},
                {}};
    case U'в':
        return {{{P(0, .25), P(.22, .6), P(.4, 1.15), P(.44, 1.58), P(.32, 1.72), P(.2, 1.52),
                  P(.21, .85), P(.27, .25), P(.42, 0), P(.62, .05), P(.72, .3), P(.58, .52),
                  P(.36, .54)},
                 {P(.36, .54), P(.6, .62), P(.82, .58), P(.98, .42)}},
                {}};
    // Отдельные буквы без входа - для мелких.
    case U'O':
        return {{{P(.55, .95), P(.3, .95), P(.12, .62), P(.15, .2), P(.38, 0), P(.62, .1),
                  P(.72, .5), P(.62, .88), P(.45, 1.0), P(.3, .95)}},
                {}};
    case U'A':
        return {{{P(.66, .82), P(.48, .98), P(.25, .88), P(.13, .48), P(.25, .06), P(.45, .02),
                  P(.62, .3), P(.7, .98)},
                 {P(.7, .98), P(.65, .45), P(.68, .08), P(.8, 0), P(.95, .18)}},
                {}};
    case U'E':
        return {{{P(.15, .45), P(.4, .52), P(.6, .7), P(.58, .92), P(.4, 1.0), P(.2, .86),
                  P(.1, .5), P(.2, .12), P(.42, 0), P(.62, .06), P(.75, .22)}},
                {}};
    // Цифры: высота 1.45.
    case U'2':
        return {{{P(.05, 1.15), P(.22, 1.4), P(.48, 1.45), P(.66, 1.25), P(.6, .9), P(.32, .45),
                  P(.03, .02)},
                 {P(.03, .02), P(.35, .04), P(.72, 0)}},
                {}};
    case U'0':
        return {{{P(.42, 1.45), P(.18, 1.3), P(.06, .72), P(.14, .12), P(.38, 0), P(.62, .14),
                  P(.7, .74), P(.6, 1.3), P(.4, 1.45), P(.28, 1.36)}},
                {}};
    case U'5':
        return {{{P(.16, 1.45), P(.12, .82)},
                 {P(.12, .82), P(.36, .95), P(.6, .86), P(.7, .46), P(.56, .08), P(.3, 0),
                  P(.04, .16)}},
                {{{P(.16, 1.45), P(.45, 1.47), P(.72, 1.45)}}}};
    // Острые углы (печатные N, M, галочка).
    case U'N':
        return {{{P(0, 0), P(0, 1)}, {P(0, 1), P(.7, 0)}, {P(.7, 0), P(.7, 1)}}, {}};
    case U'M':
        return {{{P(0, 0), P(.04, 1)}, {P(.04, 1), P(.38, .3)}, {P(.38, .3), P(.72, 1)},
                 {P(.72, 1), P(.76, 0)}},
                {}};
    case U'V':
        return {{{P(0, .45), P(.28, 0)}, {P(.28, 0), P(.9, 1.0)}}, {}};
    default:
        return {};
    }
}

struct Built
{
    std::vector<Shape> strokes;
};

double maxX(const std::vector<std::vector<QPointF>> &parts)
{
    double m = -1e9;
    for (const auto &part : parts)
        for (const QPointF &p : part)
            m = std::max(m, p.x());
    return m;
}

// Слово: буквы связаны в один штрих (connected = true) или каждая отдельно.
// Наклон slant (радианы), лёгкое случайное дрожание контрольных точек.
Built writeText(const std::u32string &text, double size, bool connected, double slant,
                double jitterEm, Rng &rng)
{
    Built built;
    Shape current;
    double cursor = 0.0;
    const double shear = std::tan(slant);
    const auto place = [&](const QPointF &em, double offset) {
        const double x = em.x() + offset + em.y() * shear;
        return QPointF(x * size, -em.y() * size);
    };
    const auto wobble = [&](const QPointF &p) {
        return p + QPointF(rng.gauss(), rng.gauss()) * jitterEm;
    };

    for (char32_t c : text) {
        if (c == U' ') {
            if (!current.parts.empty()) {
                built.strokes.push_back(current);
                current = Shape();
            }
            cursor += 0.6;
            continue;
        }
        const Glyph g = glyph(c);
        if (g.parts.empty())
            continue;
        std::vector<std::vector<QPointF>> parts;
        for (const auto &part : g.parts) {
            std::vector<QPointF> placed;
            for (size_t i = 0; i < part.size(); ++i) {
                // Края частей не дрожат: острие у соседних частей общее.
                const bool edge = i == 0 || i + 1 == part.size();
                placed.push_back(place(edge ? part[i] : wobble(part[i]), cursor));
            }
            parts.push_back(placed);
        }
        if (connected && !current.parts.empty()) {
            auto &last = current.parts.back();
            last.insert(last.end(), parts.front().begin(), parts.front().end());
            for (size_t i = 1; i < parts.size(); ++i)
                current.parts.push_back(parts[i]);
        } else {
            if (!current.parts.empty())
                built.strokes.push_back(current);
            current = Shape();
            current.parts = parts;
        }
        if (!connected) {
            built.strokes.push_back(current);
            current = Shape();
        }
        for (const auto &extra : g.extras) {
            Shape s;
            for (const auto &part : extra) {
                std::vector<QPointF> placed;
                for (const QPointF &p : part)
                    placed.push_back(place(p, cursor));
                s.parts.push_back(placed);
            }
            built.strokes.push_back(s); // после основного - так пишут точку над i
        }
        double advance = 0;
        for (const auto &part : g.parts)
            for (const QPointF &p : part)
                advance = std::max(advance, p.x());
        cursor += advance + (connected ? 0.06 : 0.25);
    }
    if (!current.parts.empty())
        built.strokes.push_back(current);

    // Точки над i - штрихи, которые пишут после слова: переставляем в конец.
    std::stable_partition(built.strokes.begin(), built.strokes.end(), [](const Shape &s) {
        double length = 0;
        for (const auto &part : s.parts)
            for (size_t i = 1; i < part.size(); ++i)
                length += len(part[i] - part[i - 1]);
        return length > 3.0;
    });
    return built;
}

// Ломаная -> части, разрезанные по поворотам больше 45°.
Shape fromPolyline(const QList<QPointF> &points)
{
    Shape shape;
    std::vector<QPointF> part;
    for (int i = 0; i < points.size(); ++i) {
        part.push_back(points[i]);
        if (i > 0 && i + 1 < points.size()) {
            const QPointF a = points[i] - points[i - 1];
            const QPointF b = points[i + 1] - points[i];
            const double turn = std::abs(std::atan2(a.x() * b.y() - a.y() * b.x(),
                                                    a.x() * b.x() + a.y() * b.y()));
            if (turn > M_PI / 4) {
                shape.parts.push_back(part);
                part = {points[i]};
            }
        }
    }
    shape.parts.push_back(part);
    return shape;
}

// Острия между частями с поворотом больше 45° - эталонные вершины углов.
void collectCorners(const Shape &shape, std::vector<QPointF> &corners)
{
    for (size_t i = 0; i + 1 < shape.parts.size(); ++i) {
        const std::vector<QPointF> a = densify(shape.parts[i]);
        const std::vector<QPointF> b = densify(shape.parts[i + 1]);
        if (a.size() < 2 || b.size() < 2)
            continue;
        // Касательные по последнему / первому пикселю.
        QPointF ta, tb;
        for (size_t k = a.size() - 1; k > 0; --k) {
            if (len(a.back() - a[k - 1]) >= 1.0 || k == 1) {
                ta = a.back() - a[k - 1];
                break;
            }
        }
        for (size_t k = 1; k < b.size(); ++k) {
            if (len(b[k] - b.front()) >= 1.0 || k + 1 == b.size()) {
                tb = b[k] - b.front();
                break;
            }
        }
        const double turn = std::abs(std::atan2(ta.x() * tb.y() - ta.y() * tb.x(),
                                                ta.x() * tb.x() + ta.y() * tb.y()));
        if (turn > M_PI / 4)
            corners.push_back(a.back());
    }
}

struct Spec
{
    std::string id;
    std::string title;
    std::vector<std::string> tags;
    double size;
    std::vector<Shape> strokes;
    Motion motion;
    // Частота пера, если не та, что у корпуса; 0 - CorpusOptions::rateHz.
    double rateHz = 0.0;
    // Эталонные вершины острий, которых нет между частями (острие внутри
    // части - разворот без остановки), в координатах фигуры.
    std::vector<QPointF> extraCorners;
};

Sample realize(const Spec &spec, uint64_t seed, const CorpusOptions &options)
{
    Rng rng(seed);
    Sample sample;
    sample.id = spec.id;
    sample.title = spec.title;
    sample.tags = spec.tags;
    sample.size = spec.size;

    // Сдвиг в положительную область с полем 12 px.
    double minX = 1e9, minY = 1e9;
    for (const Shape &s : spec.strokes)
        for (const auto &part : s.parts)
            for (const QPointF &p : part) {
                minX = std::min(minX, p.x());
                minY = std::min(minY, p.y());
            }

    CorpusOptions sampling = options;
    if (spec.rateHz > 0.0)
        sampling.rateHz = spec.rateHz;
    for (const QPointF &corner : spec.extraCorners)
        sample.corners.push_back(corner + QPointF(16.0 - minX, 16.0 - minY));

    double t = 0.0;
    for (const Shape &raw : spec.strokes) {
        Shape s = raw;
        for (auto &part : s.parts)
            for (QPointF &p : part)
                p += QPointF(16.0 - minX, 16.0 - minY);
        collectCorners(s, sample.corners);
        Motion m = spec.motion;
        // Короткий штрих (точка над i) ведётся медленнее и короче.
        SampleStroke stroke;
        stroke.truth = timeParameterize(s, m, t, rng);
        if (stroke.truth.p.size() < 2)
            continue;
        sampleEvents(stroke, m, sampling, 7300.0 + seed * 10.0, rng);
        if (stroke.events.size() < 2) {
            continue;
        }
        t = stroke.truth.t.back() + 0.18; // отрыв между штрихами
        sample.strokes.push_back(std::move(stroke));
    }

    QRectF bounds;
    for (const SampleStroke &s : sample.strokes)
        for (const QPointF &p : s.truth.p)
            bounds = bounds.isNull() ? QRectF(p, QSizeF(1e-3, 1e-3))
                                     : bounds.united(QRectF(p, QSizeF(1e-3, 1e-3)));
    sample.bounds = QRectF(0, 0, std::ceil(bounds.right() + 16.0), std::ceil(bounds.bottom() + 16.0));
    return sample;
}

// Ломаная одной частью, плотно (шаг 0.05 px): её вершины - развороты без
// остановки пера. Скорость в них падает только по закону 2/3 (до 0.35 от
// крейсерской), а не до нуля, как в остриях между частями.
Shape densePolyline(const QList<QPointF> &points)
{
    std::vector<QPointF> dense;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i];
        const QPointF b = points[i + 1];
        const int steps = std::max(1, int(std::ceil(len(b - a) / 0.05)));
        for (int k = 0; k < steps; ++k)
            dense.push_back(a + (b - a) * (double(k) / steps));
    }
    dense.push_back(points.constLast());
    Shape shape;
    shape.parts.push_back(dense);
    return shape;
}

// Зубец короче кадра - ровно эталон tst_strokesampler::toothShorterThanFrameSurvives:
// один отсчёт на 240 Гц уходит вверх на 3 px. Без тремора и шума датчика,
// метки времени - те же, что в тесте.
Sample toothSample()
{
    const reference::ToothStroke tooth = reference::toothShorterThanFrame();
    Sample sample;
    sample.id = "tooth_240";
    sample.title = "Зубец 3 px в один отсчёт, 300 px/s, 240 Гц";
    sample.tags = {"tooth"};
    sample.size = tooth.amplitude;
    SampleStroke stroke;
    double maxX = 0.0, maxY = 0.0;
    for (const InkPoint &p : tooth.points) {
        stroke.events.push_back(p);
        stroke.eventTrueTime.push_back(p.timeSeconds);
        stroke.truth.t.push_back(p.timeSeconds);
        stroke.truth.p.push_back(p.position);
        stroke.truth.pressure.push_back(p.pressure);
        stroke.truth.speed.push_back(300.0);
        maxX = std::max(maxX, p.position.x());
        maxY = std::max(maxY, p.position.y());
    }
    sample.strokes.push_back(std::move(stroke));
    sample.toothApex = tooth.apex;
    sample.toothAmplitude = tooth.amplitude;
    sample.bounds = QRectF(0, 0, std::ceil(maxX + 16.0), std::ceil(maxY + 16.0));
    return sample;
}

QList<QPointF> toQList(const reference::Cardiogram &c)
{
    QList<QPointF> path = c.screenPath;
    return path;
}

} // namespace

std::vector<Sample> buildCorpus(const CorpusOptions &options)
{
    std::vector<Spec> specs;
    const auto motion = [](double cruise, double radius, double tremor, uint64_t seed) {
        Motion m;
        m.cruise = cruise;
        m.curvatureRadius = radius;
        m.tremor = tremor;
        m.seed = seed;
        return m;
    };
    Rng shapeRng(20261003);
    const double slant = 12.0 * M_PI / 180.0;

    // Эталоны tests/support/referencestrokes.h (геометрия; скорость - своя).
    specs.push_back({"ref_line", "Прямая 300 px, 400 px/s", {"reference"}, 40,
                     {fromPolyline(reference::straightLine())}, motion(400, 20, 0.12, 1)});
    specs.push_back({"ref_loop", "Окружность r=40, 600 px/s", {"reference"}, 80,
                     {fromPolyline(reference::loop())}, motion(600, 40, 0.12, 2)});
    specs.push_back({"ref_zigzag", "Зигзаг с разворотами, 300 px/s", {"reference", "corner"}, 24,
                     {fromPolyline(reference::sharpZigzag())}, motion(300, 12, 0.12, 3)});
    specs.push_back({"ref_spiral", "Тугая спираль, 500 px/s", {"reference"}, 60,
                     {fromPolyline(reference::tightSpiral())}, motion(500, 30, 0.12, 4)});
    specs.push_back({"ref_letter_e", "Эталонная «e», 300 px/s", {"reference"}, 36,
                     {fromPolyline(reference::letterE())}, motion(300, 18, 0.12, 5)});
    specs.push_back({"ref_fast_circle", "Окружность r=30 x2, 1500 px/s", {"reference", "fast"}, 60,
                     {fromPolyline(reference::circle(30.0, 2))}, motion(1500, 30, 0.12, 6)});
    specs.push_back({"ref_corner_v", "«V», 300 px/s", {"reference", "corner"}, 80,
                     {fromPolyline(reference::cornerV())}, motion(300, 40, 0.12, 7)});
    specs.push_back({"ref_corner_n", "«N», 300 px/s", {"reference", "corner"}, 80,
                     {fromPolyline(reference::cornerN())}, motion(300, 40, 0.12, 8)});
    specs.push_back({"ref_cardiogram", "Кардиограмма 15 px, 600 px/s", {"reference", "fast"}, 15,
                     {fromPolyline(toQList(reference::cardiogram()))}, motion(600, 6, 0.12, 9)});

    const auto text = [&](const std::u32string &s, double size, bool connected) {
        return writeText(s, size, connected, slant, 0.025, shapeRng).strokes;
    };

    specs.push_back({"word_minimum", "«minimum», x-height 14, 280 px/s", {"word"}, 14,
                     text(U"minimum", 14, true), motion(280, 7, 0.15, 11)});
    specs.push_back({"word_hello", "«hello», x-height 14, 300 px/s", {"word"}, 14,
                     text(U"hello", 14, true), motion(300, 7, 0.15, 12)});
    specs.push_back({"word_privet", "«привет», x-height 14, 280 px/s", {"word"}, 14,
                     text(U"привет", 14, true), motion(280, 7, 0.15, 13)});
    specs.push_back({"digits_2025", "«2025», x-height 14, 250 px/s", {"word"}, 14,
                     text(U"2025", 14, false), motion(250, 7, 0.15, 14)});
    specs.push_back({"word_minimum_small", "«minimum», x-height 8, 180 px/s", {"word", "small"}, 8,
                     text(U"minimum", 8, true), motion(180, 4, 0.15, 15)});

    specs.push_back({"small_o_8", "«o» 8 px, 120 px/s", {"small", "closed"}, 8,
                     text(U"O", 8, false), motion(120, 4, 0.15, 21)});
    specs.push_back({"small_o_12", "«o» 12 px, 180 px/s", {"small", "closed"}, 12,
                     text(U"O", 12, false), motion(180, 6, 0.15, 22)});
    specs.push_back({"small_o_20", "«o» 20 px, 280 px/s", {"small", "closed"}, 20,
                     text(U"O", 20, false), motion(280, 10, 0.15, 23)});
    specs.push_back({"small_a_12", "«a» 12 px, 200 px/s", {"small"}, 12,
                     text(U"A", 12, false), motion(200, 6, 0.15, 24)});
    specs.push_back({"small_e_10", "«e» 10 px, 180 px/s", {"small"}, 10,
                     text(U"E", 10, false), motion(180, 5, 0.15, 25)});

    {
        // Точки-тапы: перо касается и почти не движется (0.15…3 px), каждая -
        // отдельный штрих. Точка над i, пунктуация, точка в «ё».
        std::vector<Shape> dots;
        const double lengths[] = {0.15, 0.4, 0.8, 1.5, 3.0};
        double x = 0.0;
        for (double length : lengths) {
            Shape s;
            s.parts.push_back({QPointF(x, 0.0), QPointF(x + length * 0.8, length * 0.6)});
            dots.push_back(s);
            x += 14.0;
        }
        specs.push_back({"dots_tap", "Тапы-точки 0.15…3 px, 15 px/s", {"tap"}, 8, dots,
                         motion(15, 8, 0.12, 26)});
    }

    specs.push_back({"corner_N", "Печатная N 24 px, 300 px/s", {"corner"}, 24,
                     text(U"N", 24, false), motion(300, 12, 0.12, 31)});
    specs.push_back({"corner_M", "Печатная M 20 px, 300 px/s", {"corner", "small"}, 20,
                     text(U"M", 20, false), motion(300, 10, 0.12, 32)});
    specs.push_back({"corner_check", "Галочка 20 px, 350 px/s", {"corner"}, 20,
                     text(U"V", 20, false), motion(350, 10, 0.12, 33)});

    specs.push_back({"fast_eeee", "«eeee» x-height 18, 900 px/s", {"fast"}, 18,
                     text(U"eeee", 18, true), motion(900, 9, 0.12, 41)});
    {
        // Каракули: случайное гладкое блуждание, 1200 px/s.
        Rng r(4242);
        QList<QPointF> pts;
        QPointF p(0, 0);
        double heading = 0.0;
        for (int i = 0; i < 28; ++i) {
            pts.append(p);
            heading += r.uniform(-2.2, 2.2);
            p += QPointF(std::cos(heading), std::sin(heading)) * r.uniform(14, 30);
            p.setX(std::clamp(p.x(), -60.0, 60.0));
            p.setY(std::clamp(p.y(), -40.0, 40.0));
        }
        Shape s;
        s.parts.push_back(std::vector<QPointF>(pts.begin(), pts.end()));
        specs.push_back({"fast_scribble", "Каракули, 1200 px/s", {"fast"}, 20, {s},
                         motion(1200, 10, 0.12, 42)});
    }

    {
        Shape line;
        line.parts.push_back({QPointF(0, 0), QPointF(80, 3), QPointF(160, 0)});
        specs.push_back({"slow_line_tremor", "Медленная линия 40 px/s, тремор 0.3 px", {"slow"},
                         40, {line}, motion(40, 40, 0.3, 51)});
        Shape arc;
        std::vector<QPointF> pts;
        for (int i = 0; i <= 24; ++i) {
            const double a = M_PI * i / 24.0;
            pts.push_back(QPointF(-50 * std::cos(a), -50 * std::sin(a)));
        }
        arc.parts.push_back(pts);
        specs.push_back({"slow_arc_tremor", "Медленная дуга r=50, 35 px/s, тремор 0.3 px",
                         {"slow"}, 50, {arc}, motion(35, 50, 0.3, 52)});
    }
    specs.push_back({"slow_hello", "«hello» x-height 18, 70 px/s, тремор 0.3 px", {"slow", "word"},
                     18, text(U"hello", 18, true), motion(70, 9, 0.3, 53)});

    {
        // Точки и короткие касания (точка над i, «:», «;»): сдвиг пера за
        // касание 0.3…3 px. Касание совсем без сдвига корпус не умеет (эталону
        // нужно 2 точки) - его проверяет tools/stroke-lab/variants/google_ink.cpp.
        std::vector<Shape> taps;
        const double lengths[] = {0.3, 0.6, 1.0, 1.5, 2.2, 3.0};
        for (int i = 0; i < 6; ++i) {
            Shape s;
            const QPointF a(i * 12.0, (i % 2) * 6.0);
            s.parts.push_back({a, a + QPointF(lengths[i] * 0.6, lengths[i] * 0.8)});
            taps.push_back(s);
        }
        specs.push_back({"dots_taps", "Точки-касания со сдвигом 0.3…3 px", {"dots"}, 8, taps,
                         motion(60, 4, 0.12, 61)});
    }

    // Острия без стоянки пера (гейт D6): «N» эталона, перо разворачивается на
    // ходу, на частотах 240 и 481 Гц.
    for (const double rate : {240.0, 481.0}) {
        const QList<QPointF> n = reference::cornerN();
        Spec spec{rate < 300.0 ? "corner_N_nostop_240" : "corner_N_nostop_481",
                  rate < 300.0 ? "«N» без стоянки в остриях, 600 px/s, 240 Гц"
                               : "«N» без стоянки в остриях, 600 px/s, 481 Гц",
                  {"corner", "nostop"}, 80, {densePolyline(n)},
                  motion(600, 40, 0.12, rate < 300.0 ? 34 : 35)};
        spec.rateHz = rate;
        spec.extraCorners = {n[1], n[2]};
        specs.push_back(spec);
    }

    std::vector<Sample> samples;
    for (const Spec &spec : specs)
        samples.push_back(realize(spec, spec.motion.seed, options));
    samples.push_back(toothSample());
    return samples;
}

} // namespace lab
