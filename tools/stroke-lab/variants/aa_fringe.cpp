// aa-fringe* - аналитическое сглаживание кромки на нынешней треугольной полосе
// (эксперимент E1 из плана критика, путь «бахрома на полосе»).
//
// Траектория и окончательные точки - ровно baseline (AppPipeline). Меняется
// только то, что ушло бы в вершинный и фрагментный шейдеры:
//
//  * Каждая вершина полосы знает центр стыка c, единичное направление смещения
//    n и полуширину hw (точки страницы). Вершинный шейдер выносит вершину на
//    e = 1 px дальше кромки: pos = c + n·(hwPx + e) в пикселях, и отдаёт
//    varying d = ±(hwPx + e) - знаковое расстояние поперёк штриха (+ у левой
//    стороны полосы, - у правой), varying hw = hwPx.
//  * Фрагментный шейдер, профиль box (по умолчанию): точное покрытие
//    пиксельным боксом поперёк штриха = длина пересечения отрезков
//    [|d| − hw, |d| + hw] и [−0.5, 0.5]:
//        cov = clamp(min(|d| + hw, 0.5) − max(|d| − hw, −0.5), 0, 1).
//    При hw ≥ 0.5 это clamp(hw + 0.5 − |d|, 0, 1) - обычная SDF-кромка; при
//    тонкой линии профиль - трапеция высотой 2·hw, и масса чернил на единицу
//    длины равна толщине: отдельный hairline-трюк не нужен (у clamp(0.5 − sd)
//    без него масса тонкой линии завышена, (hw+0.5)² вместо 2hw).
//    Нижний порог читаемости: hw ≥ minHw (0.15 px, то есть линия не тоньше
//    0.3 px); aa-fringe-nofloor - без порога.
//  * Профиль hairline (aa-fringe-hairline, как Skia и femtovg PR #339): при
//    толщине < 1 px штрих рисуется толщиной 1 px с alpha = max(2·hwPx, 0.3),
//    cov = clamp(hw + 0.5 − |d|) · alpha - для сравнения: масса та же, но
//    профиль шире (палатка в 2 px вместо трапеции).
//  * Торцы - полувеер вокруг точки (центр с d = 0, дуга с d = hw + e): у
//    плоского торца нет бахромы вдоль штриха, кромка торца осталась бы
//    ступенчатой. Заодно торец становится круглым, как у эталонной кисти.
//
// Смешивание:
//  * aa-fringe       - обычный over на каждый фрагмент, как будет на GPU с
//                      QSGMaterial::Blending: на складках полосы (внутренняя
//                      сторона резкого стыка) бахрома кладётся дважды.
//  * aa-fringe-max   - max покрытия внутри штриха, потом over на фон (на GPU:
//                      BlendOp::Min/Max или отдельный слой штриха). Нужен,
//                      чтобы измерить, сколько стоит двойная бахрома у over.
//
// Растеризация - эмуляция одной точки выборки в центре пикселя (без MSAA),
// правило top-left: aaraster.h. Вершины считаются в rasterize(), потому что
// вынос e задан в пикселях (на GPU - в вершинном шейдере по матрице).

#include "app_pipeline.h"

#include "aaraster.h"
#include "aa_fringe.h"

#include <QtMath>

#include <cmath>
#include <limits>

namespace {

using namespace lab;

constexpr qreal kRoundJoinThreshold = 0.35; // как в StrokeTessellator
constexpr qreal kRoundJoinStep = 0.28;
constexpr qreal kBisectorEpsilon = 0.2;
constexpr double kFringePx = 1.0;

QPointF normalOf(const QPointF &d) { return QPointF(-d.y(), d.x()); }
QPointF normalized(const QPointF &v)
{
    const qreal l = std::hypot(v.x(), v.y());
    return l < 1e-9 ? QPointF() : v / l;
}
qreal cross(const QPointF &a, const QPointF &b) { return a.x() * b.y() - a.y() * b.x(); }

// Вершина полосы с осью: позиция = c + n * hw. side: +1 левая, -1 правая,
// 0 - центр веера торца (d = 0), 2 - дуга торца (d = +).
// d = side · (hwPx + e) · dscale: dscale < 1 у вершины на биссектрисе стыка -
// её настоящее расстояние до прямых обоих соседних сегментов равно
// (hw + e)·cos(θ/2). Расстояние до прямой аффинно по позиции, поэтому если у
// всех вершин треугольника d - точное расстояние до одной и той же прямой,
// интерполяция varying точна.
struct FringeAxisVertex
{
    QPointF c;
    QPointF n;
    qreal hw = 0;
    int side = 0;
    qreal dscale = 1.0;
    // aa-fringe-rho: предел выноса внутренней вершины стыка, точки страницы, -
    // радиус кривизны осевой. У остальных вершин предела нет.
    qreal radius = std::numeric_limits<qreal>::infinity();
};

class AxisTessellator
{
public:
    AxisTessellator(const std::function<qreal(const InkPoint &)> &halfWidth, bool exactJoins,
                    bool radiusLimit = false, qreal radiusFactor = 1.0)
        : m_halfWidth(halfWidth)
        , m_exact(exactJoins)
        , m_radiusLimit(radiusLimit)
        , m_radiusFactor(radiusFactor)
    {
    }

    std::vector<FringeAxisVertex> tessellate(const QList<InkPoint> &points) const
    {
        std::vector<FringeAxisVertex> out;
        if (points.size() < 2)
            return out;
        QList<QPointF> dirs;
        QPointF prev(1.0, 0.0);
        for (int i = 0; i + 1 < points.size(); ++i) {
            QPointF d = normalized(points[i + 1].position - points[i].position);
            if (d.isNull())
                d = prev;
            dirs.append(d);
            prev = d;
        }
        out.reserve(size_t(points.size()) * 4 + 64);
        startCap(out, points.first().position, m_halfWidth(points.first()), dirs.first());
        for (int i = 1; i + 1 < points.size(); ++i) {
            const QPointF before = points[i].position - points[i - 1].position;
            const QPointF after = points[i + 1].position - points[i].position;
            joint(out, points[i].position, m_halfWidth(points[i]), dirs[i - 1], dirs[i],
                  std::max(std::hypot(before.x(), before.y()), std::hypot(after.x(), after.y())));
        }
        endCap(out, points.last().position, m_halfWidth(points.last()), dirs.last());
        return out;
    }

private:
    static int capSteps() { return qMax(4, int(std::ceil(M_PI / kRoundJoinStep))); }

    void pair(std::vector<FringeAxisVertex> &out, const QPointF &c, const QPointF &nl, const QPointF &nr,
              qreal hw) const
    {
        out.push_back({c, nl, hw, +1});
        out.push_back({c, nr, hw, -1});
    }

    void startCap(std::vector<FringeAxisVertex> &out, const QPointF &c, qreal hw, const QPointF &dir) const
    {
        // Веер от правой нормали через «назад» к левой: v(φ) = −n cosφ − dir sinφ.
        const QPointF n = normalOf(dir);
        const int steps = capSteps();
        for (int k = 0; k <= steps; ++k) {
            const qreal phi = M_PI * k / steps;
            const QPointF v = -n * std::cos(phi) - dir * std::sin(phi);
            out.push_back({c, QPointF(), hw, 0});
            out.push_back({c, normalized(v), hw, 2});
        }
        // Последняя вершина дуги совпала с левой вершиной пары; правая -
        // следом (треугольник «центр, левая, правая» вырожден по площади).
        out.push_back({c, -n, hw, -1});
    }

    void endCap(std::vector<FringeAxisVertex> &out, const QPointF &c, qreal hw, const QPointF &dir) const
    {
        const QPointF n = normalOf(dir);
        pair(out, c, n, -n, hw);
        const int steps = capSteps();
        for (int k = 0; k <= steps; ++k) {
            const qreal phi = M_PI * k / steps;
            const QPointF v = n * std::cos(phi) + dir * std::sin(phi);
            out.push_back({c, QPointF(), hw, 0});
            out.push_back({c, normalized(v), hw, 2});
        }
    }

    void joint(std::vector<FringeAxisVertex> &out, const QPointF &c, qreal hw, const QPointF &in,
               const QPointF &outDir, qreal span) const
    {
        const qreal turn = std::atan2(cross(in, outDir), QPointF::dotProduct(in, outDir));
        const qreal cosHalf = m_exact ? std::cos(turn / 2) : 1.0;
        // Предел по кривизне: внутренние вершины соседних стыков не заходят друг
        // за друга, пока вынос вдоль биссектрисы не больше
        // max(L-, L+) / (2·sin(|θ|/2)) - на равномерной нарезке это радиус
        // окружности через три точки.
        const qreal halfSine = std::sin(std::abs(turn) / 2);
        const qreal radius = m_radiusLimit && halfSine > 1e-6
            ? m_radiusFactor * span / (2.0 * halfSine)
            : std::numeric_limits<qreal>::infinity();
        if (std::abs(turn) < kRoundJoinThreshold) {
            QPointF b = normalized(normalOf(in) + normalOf(outDir));
            if (b.isNull())
                b = normalOf(outDir);
            FringeAxisVertex l{c, b, hw, +1, cosHalf};
            FringeAxisVertex r{c, -b, hw, -1, cosHalf};
            // Внутренняя сторона - та, куда поворачивает штрих.
            (turn > 0.0 ? l : r).radius = radius;
            out.push_back(l);
            out.push_back(r);
            return;
        }
        const bool left = turn > 0.0;
        const QPointF startNormal = normalOf(in) * (left ? -1.0 : 1.0);
        const QPointF endNormal = normalOf(outDir) * (left ? -1.0 : 1.0);
        const QPointF sum = normalOf(in) + normalOf(outDir);
        QPointF innerDir = normalized(sum);
        if (left)
            innerDir = -innerDir;
        const bool degenerate = std::hypot(sum.x(), sum.y()) < kBisectorEpsilon;
        const QPointF innerN = degenerate ? QPointF() : -innerDir;
        const qreal arc = std::atan2(cross(startNormal, endNormal),
                                     QPointF::dotProduct(startNormal, endNormal));
        const int steps = qMax(1, int(std::ceil(std::abs(arc) / kRoundJoinStep)));
        const auto outer = [&](int s) {
            const qreal a = arc * (qreal(s) / steps);
            const qreal co = std::cos(a), si = std::sin(a);
            return QPointF(startNormal.x() * co - startNormal.y() * si,
                           startNormal.x() * si + startNormal.y() * co);
        };
        const int innerSide = left ? +1 : -1;
        FringeAxisVertex I{c, innerN, hw, innerSide, cosHalf};
        if (!degenerate)
            I.radius = radius;
        const auto O = [&](int s) { return FringeAxisVertex{c, outer(s), hw, -innerSide, 1.0}; };
        if (!m_exact) {
            // Как прежний StrokeTessellator: веер из внутренней точки. Линейная
            // интерполяция d между внутренней и внешней вершиной, которые не
            // лежат напротив друг друга, сдвигает ноль d с оси - на остром
            // углу вылезает «шип».
            for (int s = 0; s <= steps; ++s) {
                if (left) {
                    out.push_back(I);
                    out.push_back(O(s));
                } else {
                    out.push_back(O(s));
                    out.push_back(I);
                }
            }
            return;
        }
        // Точный стык: треугольник (I, O_0, C) - всё точно относительно
        // прямой входящего сегмента; веер (O_k, C, O_k+1) вокруг центра стыка
        // C (d = 0, радиально); (C, O_K, I) - относительно исходящего.
        const FringeAxisVertex C{c, QPointF(), hw, 0, 1.0};
        if (left) {
            out.push_back(I);
            out.push_back(O(0));
            for (int s = 1; s <= steps; ++s) {
                out.push_back(C);
                out.push_back(O(s));
            }
            out.push_back(I);
            out.push_back(O(steps));
        } else {
            out.push_back(O(0));
            out.push_back(I);
            out.push_back(O(0));
            for (int s = 1; s <= steps; ++s) {
                out.push_back(C);
                out.push_back(O(s));
            }
            out.push_back(I);
        }
    }

    std::function<qreal(const InkPoint &)> m_halfWidth;
    bool m_exact = true;
    bool m_radiusLimit = false;
    qreal m_radiusFactor = 1.0;
};

class FringeVariant : public AppPipeline
{
public:
    enum class Blend { Over, Max };
    enum class Profile { Box, Hairline };
    FringeVariant(Blend blend, Profile profile, double minHalfWidthPx, bool exactJoins = true,
                  bool radiusLimit = false)
        : AppPipeline(StrokeStyle::Tapered)
        , m_blend(blend)
        , m_profile(profile)
        , m_minHw(minHalfWidthPx)
        , m_exactJoins(exactJoins)
        , m_radiusLimit(radiusLimit)
    {
    }

    std::vector<FringeAxisVertex> axisStrip() const
    {
        AxisTessellator t([this](const InkPoint &p) { return halfWidthAt(p); }, m_exactJoins,
                          m_radiusLimit, radiusFactor());
        return t.tessellate(m_final.points);
    }

    // Геометрия без бахромы: то, что лежало бы в VBO до выноса в шейдере
    // (центр + смещение), - для подсчёта вершин и эталона формы.
    Geometry geometry() const override
    {
        Geometry g;
        const std::vector<FringeAxisVertex> strip = axisStrip();
        if (strip.size() < 4)
            return g;
        g.kind = Geometry::Kind::TriangleStrip;
        g.vertices.reserve(strip.size());
        for (const FringeAxisVertex &v : strip)
            g.vertices.push_back(v.c + v.n * v.hw);
        return g;
    }

    bool rasterize(const Geometry &, const RasterTarget &target) const override
    {
        const std::vector<FringeAxisVertex> strip = axisStrip();
        if (strip.size() < 4 || !target.image)
            return false;
        const QTransform &T = target.pageToPixel;
        const double s = std::sqrt(std::abs(T.determinant()));
        // Атрибуты: d (знаковое расстояние), hw, alpha.
        std::vector<AttrVertex<3>> v;
        v.reserve(strip.size());
        for (const FringeAxisVertex &a : strip) {
            const double hwPx = a.hw * s;
            double hwDraw = std::max(hwPx, m_minHw);
            double alpha = 1.0;
            if (m_profile == Profile::Hairline && hwPx < 0.5) {
                hwDraw = 0.5;
                alpha = std::clamp(2.0 * hwPx, 0.3, 1.0);
            }
            const double ext = hwDraw + kFringePx;
            const QPointF c = T.map(a.c);
            AttrVertex<3> out;
            double d = 0;
            if (a.side == 0 || a.n.isNull()) {
                out.p = c;
                d = 0;
            } else {
                // aa-fringe-rho: внутренняя вершина стыка не дальше радиуса
                // кривизны (в пикселях). d остаётся её точным расстоянием до
                // прямых обоих сегментов: вершина лишь ближе на той же биссектрисе.
                const double reach = std::min(ext, a.radius * s);
                out.p = c + a.n * reach; // T - подобие без отражения
                d = (a.side == -1 ? -reach : reach) * a.dscale;
            }
            out.a = {float(d), float(hwDraw), float(alpha)};
            v.push_back(out);
        }
        // Знак d задан стороной вершины, а не чётностью: после стартового
        // веера (2·(K+1) вершин) правая вершина пары стоит на чётном месте.
        FragmentRaster raster(target.image->width(), target.image->height());
        AlphaBuffer buffer(target.image->width(), target.image->height());
        const bool over = m_blend == Blend::Over;
        const bool box = m_profile == Profile::Box;
        const float inkAlpha = float(target.color.alphaF());
        raster.strip(v, [&](int x, int y, const std::array<float, 3> &a) {
            const float ad = std::abs(a[0]);
            const float hw = a[1];
            const float cov = box ? std::clamp(std::min(ad + hw, 0.5f) - std::max(ad - hw, -0.5f),
                                               0.0f, 1.0f)
                                  : std::clamp(hw + 0.5f - ad, 0.0f, 1.0f) * a[2];
            if (cov <= 0.0f)
                return;
            // Альфа цвета - в каждый фрагмент, как у смешивания GPU.
            if (over)
                buffer.over(x, y, cov * inkAlpha);
            else
                buffer.max(x, y, cov * inkAlpha);
        });
        QColor opaque = target.color;
        opaque.setAlpha(255);
        buffer.compositeOnto(*target.image, opaque);
        return true;
    }

private:
    // Множитель радиуса для разбора провала гейта (AA_RHO_FACTOR, по умолчанию 1).
    // Гейт гоняется с 1: подбор множителя - решение пользователя, не гейта.
    static qreal radiusFactor()
    {
        static const qreal factor = [] {
            bool ok = false;
            const qreal value = qEnvironmentVariable("AA_RHO_FACTOR").toDouble(&ok);
            return ok && value > 0.0 ? value : 1.0;
        }();
        return factor;
    }

    Blend m_blend;
    Profile m_profile;
    double m_minHw;
    bool m_exactJoins;
    bool m_radiusLimit = false;
};

class FringeOver : public FringeVariant
{
public:
    FringeOver() : FringeVariant(Blend::Over, Profile::Box, 0.15) {}
};
class FringeMax : public FringeVariant
{
public:
    FringeMax() : FringeVariant(Blend::Max, Profile::Box, 0.15) {}
};
class FringeNoFloor : public FringeVariant
{
public:
    FringeNoFloor() : FringeVariant(Blend::Over, Profile::Box, 0.0) {}
};
class FringeNaiveJoin : public FringeVariant
{
public:
    FringeNaiveJoin() : FringeVariant(Blend::Over, Profile::Box, 0.15, false) {}
};
class FringeHairline : public FringeVariant
{
public:
    FringeHairline() : FringeVariant(Blend::Over, Profile::Hairline, 0.0) {}
};
class FringeRho : public FringeVariant
{
public:
    FringeRho() : FringeVariant(Blend::Over, Profile::Box, 0.15, true, true) {}
};

} // namespace

bool lab::fringeGpuStrip(const Variant &variant, std::vector<FringeGpuVertex> &out)
{
    const auto *fringe = dynamic_cast<const FringeVariant *>(&variant);
    if (!fringe)
        return false;
    out.clear();
    for (const FringeAxisVertex &a : fringe->axisStrip()) {
        const bool center = a.side == 0 || a.n.isNull();
        const float sign = a.side == -1 ? -1.0f : 1.0f;
        out.push_back({float(a.c.x()), float(a.c.y()), center ? 0.0f : float(a.n.x()),
                       center ? 0.0f : float(a.n.y()), float(a.hw),
                       center ? 0.0f : sign * float(a.dscale)});
    }
    return true;
}

STROKE_LAB_VARIANT(FringeOver, "aa-fringe",
                   "baseline-траектория; полоса с бахромой 1 px и точным покрытием боксом "
                   "поперёк штриха, линия не тоньше 0.3 px, круглые торцы-веера, over на "
                   "фрагмент (QSGMaterial с Blending), без MSAA");
STROKE_LAB_VARIANT(FringeMax, "aa-fringe-max",
                   "как aa-fringe, но покрытие внутри штриха объединяется по max "
                   "(BlendOp::Min/Max или слой штриха)");
STROKE_LAB_VARIANT(FringeNoFloor, "aa-fringe-nofloor",
                   "как aa-fringe, без нижнего порога толщины 0.3 px");
STROKE_LAB_VARIANT(FringeNaiveJoin, "aa-fringe-naive",
                   "как aa-fringe, но стык как в прежнем StrokeTessellator (веер из внутренней точки, "
                   "d = ±(hw+e) у всех вершин) - даёт шипы на острых углах");
STROKE_LAB_VARIANT(FringeHairline, "aa-fringe-hairline",
                   "как aa-fringe, но тонкие линии - hairline 1 px с alpha = max(w, 0.3) "
                   "(Skia/femtovg) вместо точного бокса");
STROKE_LAB_VARIANT(FringeRho, "aa-fringe-rho",
                   "как aa-fringe, но вынос внутренней вершины стыка ограничен радиусом "
                   "кривизны осевой max(L-, L+) / (2·sin(|θ|/2)) - против складки бахромы на "
                   "тугих петлях (design.md, D7)");
