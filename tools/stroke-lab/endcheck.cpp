// --endcheck: синтетика для концов штриха (apps-survey, эксперимент 1).
//
// Прямая по оси x на 100 / 300 / 600 экранных px/s, 481 Гц, метки кратны
// 1 мс, без шума. Давление: нарастание 0.05 -> 0.6 за 15 мс (smoothstep),
// плато, спад до 0 за 12 мс перед отрывом - профиль из плана критика. Плюс
// касание со сдвигом 0.3 px за 30 мс.
//
// Для каждого варианта:
//  entry_px  - длина дуги от начала, пока толщина не дойдёт до 90% плато;
//  exit_px   - то же от конца;
//  entry_ref / exit_ref - то же для эталона: закон 0.25 + 0.75 p на
//              настоящем давлении (без фильтра);
//  step_end  - наибольший относительный скачок толщины между соседними
//              точками осевой на последних 3 толщинах плато;
//  cap_dev   - отклонение вершин геометрии за концом штриха от окружности
//              радиуса полутолщины, в долях радиуса (0 - полудиск);
//  tap_aspect - отношение сторон охвата геометрии касания (1 - диск).

#include "endcheck.h"

#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lab {

namespace {

double sstep(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

struct Synthetic
{
    std::vector<InkPoint> events;
    double duration = 0.0;
};

double pressureProfile(double t, double duration)
{
    const double rin = sstep(t / 0.015);
    const double rout = sstep((duration - t) / 0.012);
    return 0.05 + (0.6 - 0.05) * rin * rout - 0.05 * (1.0 - rout);
}

Synthetic line(double speed, double length)
{
    Synthetic s;
    s.duration = length / speed;
    const double period = 1.0 / 481.0;
    for (int k = 0;; ++k) {
        const double t = k * period;
        if (t > s.duration)
            break;
        const double stamp = std::floor((1.0 + t) / 0.001 + 1e-9) * 0.001;
        s.events.push_back({QPointF(10.0 + speed * t, 20.0), std::clamp(pressureProfile(t, s.duration), 0.0, 1.0), stamp});
    }
    return s;
}

Synthetic tap()
{
    Synthetic s;
    s.duration = 0.03;
    const double period = 1.0 / 481.0;
    for (int k = 0;; ++k) {
        const double t = k * period;
        if (t > s.duration)
            break;
        const double stamp = std::floor((1.0 + t) / 0.001 + 1e-9) * 0.001;
        s.events.push_back({QPointF(10.0 + 0.3 * t / s.duration, 20.0), 0.5, stamp});
    }
    return s;
}

double arcUntil(const std::vector<CenterPoint> &c, double threshold, bool fromEnd)
{
    double arc = 0.0;
    const int n = int(c.size());
    for (int k = 0; k < n; ++k) {
        const int i = fromEnd ? n - 1 - k : k;
        if (c[size_t(i)].width >= threshold)
            return arc;
        const int j = fromEnd ? i - 1 : i + 1;
        if (j < 0 || j >= n)
            break;
        arc += std::hypot(c[size_t(j)].pos.x() - c[size_t(i)].pos.x(), c[size_t(j)].pos.y() - c[size_t(i)].pos.y());
    }
    return arc;
}

} // namespace

int runEndCheck(const std::vector<const VariantInfo *> &variants, double width)
{
    std::printf("\n## endcheck: концы штриха на синтетике (ширина %.2f pt, масштаб 1)\n\n", width);
    std::printf("| variant | v px/s | entry px | entry ref | exit px | exit ref | entry/exit | step_end | cap_dev |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (const VariantInfo *info : variants) {
        for (double speed : {100.0, 300.0, 600.0}) {
            const Synthetic s = line(speed, std::max(60.0, speed * 0.2));
            std::unique_ptr<Variant> v = info->factory();
            StrokeSetup setup;
            setup.scale = 1.0;
            setup.width = width;
            v->begin(setup);
            for (const InkPoint &e : s.events)
                v->addEvent(e);
            v->finish();
            const std::vector<CenterPoint> c = v->centerline();
            if (c.size() < 3)
                continue;
            const double plateau = c[c.size() / 2].width;
            const double entry = arcUntil(c, 0.9 * plateau, false);
            const double exitLen = arcUntil(c, 0.9 * plateau, true);
            // Эталон: закон приложения на настоящем давлении.
            std::vector<CenterPoint> ref;
            for (const InkPoint &e : s.events)
                ref.push_back({e.position, width * (0.25 + 0.75 * e.pressure), e.pressure, e.timeSeconds});
            const double refPlateau = ref[ref.size() / 2].width;
            const double entryRef = arcUntil(ref, 0.9 * refPlateau, false);
            const double exitRef = arcUntil(ref, 0.9 * refPlateau, true);
            // Скачок толщины на последних 3 толщинах.
            double step = 0.0, arc = 0.0;
            for (size_t i = c.size() - 1; i > 0 && arc < 3.0 * plateau; --i) {
                const double w0 = c[i - 1].width, w1 = c[i].width;
                step = std::max(step, std::abs(w1 - w0) / std::max(w0, 1e-9));
                arc += std::hypot(c[i].pos.x() - c[i - 1].pos.x(), c[i].pos.y() - c[i - 1].pos.y());
            }
            // Торец: вершины геометрии за концом осевой.
            const Geometry g = v->geometry();
            const QPointF end = c.back().pos;
            const double r = c.back().width / 2.0;
            double capDev = 0.0;
            int beyond = 0;
            std::vector<QPointF> verts = g.vertices;
            for (const QPolygonF &poly : g.polygons)
                verts.insert(verts.end(), poly.begin(), poly.end());
            for (const QPointF &p : verts) {
                if (p.x() <= end.x() + 1e-6)
                    continue;
                ++beyond;
                capDev = std::max(capDev, std::abs(std::hypot(p.x() - end.x(), p.y() - end.y()) - r) / r);
            }
            if (beyond == 0)
                capDev = 1.0; // плоский торец: ничего за концом
            std::printf("| %s | %.0f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.3f |\n", info->name.c_str(),
                        speed, entry, entryRef, exitLen, exitRef, exitLen > 0 ? entry / exitLen : 0.0, step,
                        capDev);
        }
    }
    std::printf("\n| variant | tap: points | tap bbox w x h, pt | aspect |\n|---|---|---|---|\n");
    for (const VariantInfo *info : variants) {
        const Synthetic s = tap();
        std::unique_ptr<Variant> v = info->factory();
        StrokeSetup setup;
        setup.width = width;
        v->begin(setup);
        for (const InkPoint &e : s.events)
            v->addEvent(e);
        v->finish();
        const Geometry g = v->geometry();
        QRectF box;
        bool first = true;
        std::vector<QPointF> verts = g.vertices;
        for (const QPolygonF &poly : g.polygons)
            verts.insert(verts.end(), poly.begin(), poly.end());
        for (const QPointF &p : verts) {
            if (first)
                box = QRectF(p, QSizeF(0, 0));
            box = box.united(QRectF(p, QSizeF(1e-9, 1e-9)));
            first = false;
        }
        std::printf("| %s | %zu | %.2f x %.2f | %.2f |\n", info->name.c_str(), v->centerline().size(),
                    box.width(), box.height(), box.height() > 0 ? box.width() / box.height() : 0.0);
    }

    // Траектория (эксперимент 2): окружность r = 20 px на 50/100/150 px/s,
    // 481 Гц, метки 1 мс, КОРРЕЛИРОВАННЫЙ шум AR(1) rho = 0.6, sigma = 0.3 px
    // по оси. residual - СКО радиального отклонения осевой от настоящей
    // окружности; rough - СКО второй разности угла касательной на нарезке
    // 1 px (град). Без 15 px у концов. Среднее по 5 зёрнам.
    std::printf("\n| variant | v px/s | residual px | rough deg | radius bias px |\n|---|---|---|---|---|\n");
    for (const VariantInfo *info : variants) {
        for (double speed : {50.0, 100.0, 150.0}) {
            double resSum = 0, roughSum = 0, biasSum = 0;
            const int seeds = 5;
            for (int seed = 1; seed <= seeds; ++seed) {
                uint64_t state = 0x9E3779B97F4A7C15ULL * uint64_t(seed);
                const auto gauss = [&state] {
                    auto next = [&state] {
                        state ^= state << 13;
                        state ^= state >> 7;
                        state ^= state << 17;
                        return double(state >> 11) / double(1ULL << 53);
                    };
                    const double u1 = std::max(next(), 1e-12), u2 = next();
                    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
                };
                const double r = 20.0, rho = 0.6, sigma = 0.3;
                const QPointF c(40.0, 40.0);
                const double duration = 1.6 * M_PI * r / speed; // 0.8 оборота
                std::unique_ptr<Variant> v = info->factory();
                StrokeSetup setup;
                setup.width = width;
                v->begin(setup);
                double nx = 0, ny = 0;
                for (int k = 0;; ++k) {
                    const double t = k / 481.0;
                    if (t > duration)
                        break;
                    nx = rho * nx + std::sqrt(1 - rho * rho) * sigma * gauss();
                    ny = rho * ny + std::sqrt(1 - rho * rho) * sigma * gauss();
                    const double a = speed * t / r;
                    const double stamp = std::floor((1.0 + t) / 0.001 + 1e-9) * 0.001;
                    v->addEvent({c + QPointF(r * std::cos(a) + nx, r * std::sin(a) + ny), 0.5, stamp});
                }
                v->finish();
                const std::vector<CenterPoint> cl = v->centerline();
                // Нарезка 1 px.
                std::vector<QPointF> u;
                u.push_back(cl.front().pos);
                double carry = 0;
                for (size_t i = 1; i < cl.size(); ++i) {
                    QPointF a = cl[i - 1].pos;
                    const QPointF b = cl[i].pos;
                    double seg = std::hypot(b.x() - a.x(), b.y() - a.y());
                    while (carry + seg >= 1.0) {
                        const double need = 1.0 - carry;
                        a = a + (b - a) * (need / seg);
                        seg -= need;
                        carry = 0;
                        u.push_back(a);
                    }
                    carry += seg;
                }
                double res = 0, bias = 0, rough = 0;
                int nres = 0, nrough = 0;
                std::vector<double> th;
                for (size_t i = 0; i + 1 < u.size(); ++i)
                    th.push_back(std::atan2(u[i + 1].y() - u[i].y(), u[i + 1].x() - u[i].x()));
                for (size_t i = 15; i + 15 < u.size(); ++i) {
                    const double d = std::hypot(u[i].x() - c.x(), u[i].y() - c.y()) - r;
                    res += d * d;
                    bias += d;
                    ++nres;
                }
                for (size_t i = 16; i + 16 < th.size(); ++i) {
                    auto wrap = [](double x) { return std::remainder(x, 2 * M_PI); };
                    const double d2 = wrap(th[i + 1] - th[i]) - wrap(th[i] - th[i - 1]);
                    rough += d2 * d2;
                    ++nrough;
                }
                resSum += std::sqrt(res / std::max(nres, 1));
                biasSum += bias / std::max(nres, 1);
                roughSum += std::sqrt(rough / std::max(nrough, 1)) * 180.0 / M_PI;
            }
            std::printf("| %s | %.0f | %.3f | %.2f | %.3f |\n", info->name.c_str(), speed, resSum / seeds,
                        roughSum / seeds, biasSum / seeds);
        }
    }
    return 0;
}

} // namespace lab
