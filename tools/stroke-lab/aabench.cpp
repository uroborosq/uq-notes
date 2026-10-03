// stroke-lab-aabench: качество кромки тонкой прямой линии при разных способах
// растеризации (exp/dry-refine, эксперимент 2 из плана критика).
//
// Прямая полоса толщины w (device px) под углом θ проходит через всё
// изображение. Способы:
//   msaa4 / msaa8 / msaa16 - точки выборки стандартных шаблонов, полоса из двух
//     треугольников, resolve - среднее по точкам (как в приложении, без гаммы);
//   an-ink - покрытие clamp(r + 0.5 − d, 0, 1) по расстоянию до осевой; тоньше
//     1 px - ширина 1 px и альфа 2r (google/ink: минимальная ширина с
//     масштабированием прозрачности);
//   an-box - |[d − r, d + r] ∩ [−½, ½]|: точный бокс-фильтр для полосы вдоль оси;
//   an-trap - то же, но пиксель проецируется на нормаль как трапеция: точная
//     площадь для прямой полосы под любым углом (нужен только наклон нормали -
//     в шейдере это производная расстояния);
//   reference - точная площадь пересечения пикселя с полосой (численно, 64×64
//     подвыборок на пиксель).
// Метрики по пикселям в полосе |d| <= r + 2 вдали от краёв изображения:
//   rms - СКО ошибки покрытия относительно reference;
//   cv  - «бусинистость»: коэффициент вариации суммы покрытия по столбцу
//     (поперечный срез линии) вдоль линии; у reference ≈ 0;
//   peak - перепад наибольшего покрытия пикселя в срезе (max − min вдоль линии):
//     то, что глаз видит как тёмные точки и бледные провалы.
//
// Запуск: stroke-lab-aabench [--out DIR] - печатает таблицу, пишет aabench.json и
// aabench.png (линии ×8).

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "raster.h"

namespace {

constexpr int kSize = 160;

struct Line
{
    double width;  // device px
    double angle;  // градусы
    double offset; // сдвиг осевой по нормали, px (доля пикселя)
    QPointF n() const
    {
        const double a = angle * M_PI / 180.0;
        return QPointF(-std::sin(a), std::cos(a));
    }
    QPointF dir() const
    {
        const double a = angle * M_PI / 180.0;
        return QPointF(std::cos(a), std::sin(a));
    }
    QPointF center() const { return QPointF(kSize / 2.0, kSize / 2.0) + n() * offset; }
    double dist(const QPointF &p) const
    {
        const QPointF d = p - center();
        return d.x() * n().x() + d.y() * n().y();
    }
};

using Coverage = std::vector<double>;

Coverage reference(const Line &l)
{
    Coverage c(kSize * kSize, 0.0);
    const int sub = 64;
    const double r = l.width / 2.0;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            if (std::abs(l.dist(QPointF(x + 0.5, y + 0.5))) > r + 1.0)
                continue;
            int in = 0;
            for (int j = 0; j < sub; ++j)
                for (int i = 0; i < sub; ++i)
                    if (std::abs(l.dist(QPointF(x + (i + 0.5) / sub, y + (j + 0.5) / sub))) <= r)
                        ++in;
            c[size_t(y * kSize + x)] = double(in) / (sub * sub);
        }
    return c;
}

lab::SamplePattern pattern(int n)
{
    static const int o8[8][2] = {{1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
    static const int o16[16][2] = {{1, 1}, {-1, -3}, {-3, 2}, {4, -1}, {-5, -2}, {2, 5}, {5, 3}, {3, -5},
                                   {-2, 6}, {0, -7}, {-4, -6}, {-6, 4}, {-8, 0}, {7, -4}, {6, 7}, {-7, -8}};
    if (n == 4)
        return lab::SamplePattern::msaa4();
    lab::SamplePattern p;
    for (int i = 0; i < n; ++i) {
        const int *o = n == 8 ? o8[i] : o16[i];
        p.offsets.emplace_back(0.5 + o[0] / 16.0, 0.5 + o[1] / 16.0);
    }
    return p;
}

Coverage msaa(const Line &l, int n)
{
    lab::CoverageBuffer buf(kSize, kSize, pattern(n));
    const QPointF c = l.center(), d = l.dir() * (kSize * 2.0), nn = l.n() * (l.width / 2.0);
    buf.addConvex({c - d + nn, c + d + nn, c + d - nn, c - d - nn});
    Coverage out(kSize * kSize);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            out[size_t(y * kSize + x)] = buf.coverage(x, y);
    return out;
}

// google/ink: ширина не меньше 1 px, альфа 2r.
Coverage analyticInk(const Line &l)
{
    Coverage out(kSize * kSize);
    const double r = l.width / 2.0;
    const double re = std::max(r, 0.5);
    const double alpha = std::min(1.0, 2.0 * r);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const double d = std::abs(l.dist(QPointF(x + 0.5, y + 0.5)));
            out[size_t(y * kSize + x)] = std::clamp(re + 0.5 - d, 0.0, 1.0) * alpha;
        }
    return out;
}

// Покрытие пикселя полосой |x − d| <= r, где x - координата по нормали n:
// проекция единичного квадрата на n - трапеция с опорой [−a, a],
// a = (|nx| + |ny|)/2, и плато [−b, b], b = ||nx| − |ny||/2. Покрытие -
// F(d + r) − F(d − r), F - функция распределения трапеции. Вдоль оси (a = b =
// ½) это ровно бокс-фильтр; на 45° учитывает, что пиксель по нормали шире.
inline double trapezoidCdf(double x, double a, double b)
{
    const double h = 1.0 / (a + b);
    if (x <= -a)
        return 0.0;
    if (x >= a)
        return 1.0;
    if (x < -b)
        return h * (x + a) * (x + a) / (2.0 * (a - b));
    if (x <= b)
        return h * (a - b) / 2.0 + h * (x + b);
    return 1.0 - h * (a - x) * (a - x) / (2.0 * (a - b));
}

inline double stripCoverage(double d, double r, double nx, double ny)
{
    const double ax = std::abs(nx), ay = std::abs(ny);
    const double a = (ax + ay) / 2.0, b = std::abs(ax - ay) / 2.0;
    return std::max(0.0, trapezoidCdf(d + r, a, b) - trapezoidCdf(d - r, a, b));
}

// Трапеция: точная площадь для бесконечной прямой полосы.
Coverage analyticTrap(const Line &l)
{
    Coverage out(kSize * kSize);
    const double r = l.width / 2.0;
    const QPointF n = l.n();
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            out[size_t(y * kSize + x)] = stripCoverage(l.dist(QPointF(x + 0.5, y + 0.5)), r, n.x(), n.y());
    return out;
}

// Бокс-фильтр по расстоянию: |[d − r, d + r] ∩ [−½, ½]|.
Coverage analyticBox(const Line &l)
{
    Coverage out(kSize * kSize);
    const double r = l.width / 2.0;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const double d = std::abs(l.dist(QPointF(x + 0.5, y + 0.5)));
            out[size_t(y * kSize + x)] = std::max(0.0, std::min(d + r, 0.5) - std::max(d - r, -0.5));
        }
    return out;
}

struct Score
{
    double rms = 0, cv = 0, peak = 0;
};

Score score(const Line &l, const Coverage &c, const Coverage &ref)
{
    const double r = l.width / 2.0;
    double se = 0;
    int n = 0;
    std::vector<double> sums, peaks;
    for (int x = 20; x < kSize - 20; ++x) {
        double s = 0, pk = 0;
        for (int y = 0; y < kSize; ++y) {
            const QPointF p(x + 0.5, y + 0.5);
            if (std::abs(l.dist(p)) > r + 2.0)
                continue;
            const double v = c[size_t(y * kSize + x)];
            const double e = v - ref[size_t(y * kSize + x)];
            se += e * e;
            ++n;
            s += v;
            pk = std::max(pk, v);
        }
        sums.push_back(s);
        peaks.push_back(pk);
    }
    Score out;
    out.rms = n ? std::sqrt(se / n) : 0;
    double mean = 0;
    for (double s : sums)
        mean += s;
    mean /= std::max<size_t>(1, sums.size());
    double var = 0;
    for (double s : sums)
        var += (s - mean) * (s - mean);
    var /= std::max<size_t>(1, sums.size());
    out.cv = mean > 0 ? std::sqrt(var) / mean : 0;
    out.peak = *std::max_element(peaks.begin(), peaks.end()) - *std::min_element(peaks.begin(), peaks.end());
    return out;
}

QImage render(const Coverage &c, int x0, int y0, int w, int h, int zoom)
{
    const QColor ink(0x1c, 0x2a, 0x5a);
    QImage img(w * zoom, h * zoom, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double v = c[size_t((y0 + y) * kSize + x0 + x)];
            const QRgb px = qRgb(int(255 + (ink.red() - 255) * v), int(255 + (ink.green() - 255) * v),
                                 int(255 + (ink.blue() - 255) * v));
            for (int j = 0; j < zoom; ++j)
                for (int i = 0; i < zoom; ++i)
                    img.setPixel(x * zoom + i, y * zoom + j, px);
        }
    return img;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString out = QStringLiteral("aabench-out");
    const QStringList args = app.arguments();
    const int i = int(args.indexOf(QStringLiteral("--out")));
    if (i >= 0 && i + 1 < args.size())
        out = args[i + 1];
    QDir().mkpath(out);

    const std::vector<double> widths{0.25, 0.4, 0.6, 0.8, 1.0, 1.25, 1.5, 2.0, 3.0};
    const std::vector<double> angles{0, 3, 7, 12, 20, 30, 45};
    const std::vector<std::pair<const char *, std::function<Coverage(const Line &)>>> methods{
        {"msaa4", [](const Line &l) { return msaa(l, 4); }},
        {"msaa8", [](const Line &l) { return msaa(l, 8); }},
        {"msaa16", [](const Line &l) { return msaa(l, 16); }},
        {"an-ink", [](const Line &l) { return analyticInk(l); }},
        {"an-box", [](const Line &l) { return analyticBox(l); }},
        {"an-trap", [](const Line &l) { return analyticTrap(l); }},
    };

    QJsonArray rows;
    std::printf("%-6s", "w px");
    for (const auto &m : methods)
        std::printf(" | %-8s rms   cv%%  peak", m.first);
    std::printf("\n");
    for (double w : widths) {
        std::vector<Score> agg(methods.size());
        int count = 0;
        for (double a : angles) {
            for (double off : {0.0, 0.25, 0.5}) {
                const Line l{w, a, off};
                const Coverage ref = reference(l);
                for (size_t m = 0; m < methods.size(); ++m) {
                    const Score s = score(l, methods[m].second(l), ref);
                    agg[m].rms += s.rms;
                    agg[m].cv = std::max(agg[m].cv, s.cv);
                    agg[m].peak = std::max(agg[m].peak, s.peak);
                }
                ++count;
            }
        }
        std::printf("%-6.2f", w);
        QJsonObject row;
        row["width_px"] = w;
        for (size_t m = 0; m < methods.size(); ++m) {
            agg[m].rms /= count;
            std::printf(" | %-8s %.3f %5.1f %5.2f", "", agg[m].rms, agg[m].cv * 100, agg[m].peak);
            QJsonObject o;
            o["rms_mean"] = agg[m].rms;
            o["cv_max_pct"] = agg[m].cv * 100;
            o["peak_range_max"] = agg[m].peak;
            row[methods[m].first] = o;
        }
        std::printf("\n");
        rows.append(row);
    }
    QFile f(out + QStringLiteral("/aabench.json"));
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(rows).toJson());

    // Картинка: строки - ширины 0.4/0.6/1.0/1.5, столбцы - способы, линия 7°.
    const int zoom = 8, cw = 48, ch = 14;
    const std::vector<double> showW{0.4, 0.6, 1.0, 1.5};
    QImage sheet((cw * zoom + 8) * int(methods.size() + 1) + 8, (ch * zoom + 8) * int(showW.size()) + 8,
                 QImage::Format_RGB32);
    sheet.fill(QColor(200, 200, 205));
    QPainter p(&sheet);
    for (size_t r = 0; r < showW.size(); ++r) {
        const Line l{showW[r], 7.0, 0.25};
        std::vector<Coverage> cs{reference(l)};
        for (const auto &m : methods)
            cs.push_back(m.second(l));
        for (size_t c = 0; c < cs.size(); ++c)
            p.drawImage(QPoint(8 + int(c) * (cw * zoom + 8), 8 + int(r) * (ch * zoom + 8)),
                        render(cs[c], kSize / 2 - cw / 2, kSize / 2 - ch / 2, cw, ch, zoom));
    }
    p.end();
    sheet.save(out + QStringLiteral("/aabench.png"));
    std::fprintf(stderr, "columns: reference, msaa4, msaa8, msaa16, an-ink, an-box, an-trap; rows: w=0.4,0.6,1.0,1.5 px, 7°\n");
    return 0;
}
