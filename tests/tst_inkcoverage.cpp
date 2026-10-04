// ink-beauty-pass, D7: покрытие кромки по формулам материала чернил на CPU -
// зеркало шейдеров src/canvas/shaders/ink.{vert,frag} (src/ink/inkcoverage.h).
// Пол толщины - параметр материала, а не геометрии: та же вершина даёт разную
// ширину при разном поле. Сценарий «Медленная прокрутка» - здесь же: штрих
// растеризуется зеркалом (tests/support/inkraster.h) со сдвигом на доли
// device px, и кромка от сдвига почти не меняется.

#include <QTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"
#include "support/inkraster.h"

namespace {

// Масса чернил поперёк штриха - интеграл покрытия по d. Покрытие не нулевое
// до |d| = hw + 0.5, отсюда пределы.
qreal massAcross(qreal halfWidthPx)
{
    constexpr qreal step = 0.0005;
    const qreal limit = halfWidthPx + 1.0;
    qreal mass = 0.0;
    for (qreal d = -limit; d <= limit; d += step)
        mass += ink::coverage(d, halfWidthPx) * step;
    return mass;
}

AxisVertex vertex(const QPointF &c, const QPointF &n, float hw, float s)
{
    return {float(c.x()), float(c.y()), float(n.x()), float(n.y()), hw, s};
}

// --- Медленная прокрутка ----------------------------------------------------
//
// Кадр при медленной прокрутке - та же страница со сдвигом на долю device px.
// Мерцание кромки - то, насколько от сдвига к сдвигу меняется ошибка растра
// против точного покрытия формы: метрика aa_flicker стенда
// (tools/stroke-lab/rastermetrics.h), здесь на четырёх сдвигах 0, 0.25, 0.5 и
// 0.75 px.

constexpr int kShifts = 4;

// Пороги. Мерцание - aa_flicker: на корпусе стенда при dpr 1.75 у app-ink
// 4.79 из 255 на масштабе 1 и 4.06 на 2.5, у прежней полосы под MSAA 4x -
// 18.04 и 15.33 (tasks.md, 14.7). Порог 6 - уровень app-ink с запасом в
// четверть и втрое ниже MSAA; здесь у зеркала 0.6-3.3, у MSAA 4x 11.6-15.3.
constexpr qreal kMaxFlicker = 6.0;
// Масса строки (столбца) - то, что глаз видит как «дыхание» линии при
// прокрутке. У точного покрытия она от сдвига не зависит вовсе; у зеркала
// здесь гуляет до 1.1%, у MSAA 4x на пологой линии - на 15-80%.
constexpr qreal kMaxMassBreath = 0.03;

struct Scene
{
    std::vector<QPointF> axis; // осевая, device px без сдвига
    qreal halfWidthPx = 0.0;
    int width = 0;
    int height = 0;
};

// Покрытие формы - круглой кисти постоянной толщины вдоль осевой - выборками
// в точках внутри пикселя: samples - точки в долях пикселя. Выборка внутри,
// если она ближе hw к какому-нибудь сегменту осевой; каждый сегмент
// проверяет только пиксели у себя.
std::vector<float> shapeCoverage(const Scene &scene, const QPointF &shift,
                                 const std::vector<QPointF> &samples)
{
    const size_t ns = samples.size();
    const size_t npx = size_t(scene.width) * size_t(scene.height);
    std::vector<quint8> inside(npx * ns, 0);
    const qreal hw = scene.halfWidthPx;
    for (size_t i = 0; i + 1 < scene.axis.size(); ++i) {
        const QPointF a = scene.axis[i] + shift;
        const QPointF b = scene.axis[i + 1] + shift;
        const QPointF ab = b - a;
        const qreal ll = QPointF::dotProduct(ab, ab);
        const int x0 = std::max(0, int(std::floor(std::min(a.x(), b.x()) - hw - 1)));
        const int x1 = std::min(scene.width - 1, int(std::ceil(std::max(a.x(), b.x()) + hw + 1)));
        const int y0 = std::max(0, int(std::floor(std::min(a.y(), b.y()) - hw - 1)));
        const int y1 = std::min(scene.height - 1, int(std::ceil(std::max(a.y(), b.y()) + hw + 1)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                quint8 *hit = &inside[(size_t(y) * size_t(scene.width) + size_t(x)) * ns];
                for (size_t k = 0; k < ns; ++k) {
                    if (hit[k])
                        continue;
                    const QPointF p = QPointF(x, y) + samples[k];
                    const qreal t = ll < 1e-18
                        ? 0.0
                        : std::clamp(QPointF::dotProduct(p - a, ab) / ll, 0.0, 1.0);
                    const QPointF q = a + ab * t - p;
                    hit[k] = QPointF::dotProduct(q, q) <= hw * hw ? 1 : 0;
                }
            }
        }
    }
    std::vector<float> out(npx, 0.0f);
    for (size_t i = 0; i < npx; ++i) {
        int count = 0;
        for (size_t k = 0; k < ns; ++k)
            count += inside[i * ns + k];
        out[i] = float(count) / float(ns);
    }
    return out;
}

// Точное покрытие - решётка 16x16 (ошибка эталона ~1/256).
std::vector<QPointF> gridSamples(int n)
{
    std::vector<QPointF> samples;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            samples.push_back(QPointF((i + 0.5) / n, (j + 0.5) / n));
    return samples;
}

// Стандартный узор MSAA 4x (D3D/Vulkan, повёрнутая решётка): прежняя
// отрисовка полосы без бахромы - для сравнения.
std::vector<QPointF> msaa4Samples()
{
    return {QPointF(0.375, 0.125), QPointF(0.875, 0.375), QPointF(0.125, 0.625),
            QPointF(0.625, 0.875)};
}

struct Flicker
{
    qreal flicker = 0.0;   // aa_flicker, из 255
    qreal massBreath = 0.0; // наибольший размах массы столбца (строки), доля средней
};

// render(shift) - растр при сдвиге; сдвиги - вдоль direction.
Flicker measureFlicker(const Scene &scene, const QPointF &direction,
                       const std::function<std::vector<float>(const QPointF &)> &render)
{
    const std::vector<QPointF> exact = gridSamples(16);
    std::vector<std::vector<float>> errs;
    std::vector<std::vector<qreal>> lineMass; // поперёк сдвига: столбцы при сдвиге по y
    const bool vertical = qAbs(direction.y()) > qAbs(direction.x());
    const int lines = vertical ? scene.width : scene.height;
    for (int k = 0; k < kShifts; ++k) {
        const QPointF shift = direction * (qreal(k) / kShifts);
        const std::vector<float> cov = render(shift);
        const std::vector<float> shape = shapeCoverage(scene, shift, exact);
        std::vector<float> err(cov.size());
        std::vector<qreal> mass(size_t(lines), 0.0);
        for (int y = 0; y < scene.height; ++y) {
            for (int x = 0; x < scene.width; ++x) {
                const size_t i = size_t(y) * size_t(scene.width) + size_t(x);
                err[i] = cov[i] - shape[i];
                mass[size_t(vertical ? x : y)] += cov[i];
            }
        }
        errs.push_back(std::move(err));
        lineMass.push_back(std::move(mass));
    }

    Flicker result;
    qreal acc = 0.0;
    int n = 0;
    for (size_t i = 0; i < errs.front().size(); ++i) {
        qreal mean = 0.0;
        bool any = false;
        for (const auto &e : errs) {
            mean += e[i];
            any = any || std::abs(e[i]) > 0.002f;
        }
        if (!any)
            continue;
        mean /= kShifts;
        qreal var = 0.0;
        for (const auto &e : errs)
            var += (e[i] - mean) * (e[i] - mean);
        acc += std::sqrt(var / kShifts) * 255.0;
        ++n;
    }
    result.flicker = n ? acc / n : 0.0;

    // Масса столбца при сдвиге по y (строки - при сдвиге по x) у точного
    // покрытия от сдвига не зависит: это площадь формы в полосе столбца.
    // Столбцы у торцов не в счёт: кромку торца поперёк сдвига шейдер
    // сглаживает веером, а не боксом пикселя, и там масса мала - доля от неё
    // говорит о форме торца, а не о мерцании линии.
    const auto nearEnd = [&](int line) {
        for (const QPointF &end : {scene.axis.front(), scene.axis.back()}) {
            const qreal at = vertical ? end.x() : end.y();
            if (qAbs(line + 0.5 - at) <= scene.halfWidthPx + 2.0)
                return true;
        }
        return false;
    };
    qreal peak = 0.0;
    for (int line = 0; line < lines; ++line)
        peak = std::max(peak, lineMass.front()[size_t(line)]);
    for (int line = 0; line < lines; ++line) {
        if (nearEnd(line))
            continue;
        qreal lo = std::numeric_limits<qreal>::max();
        qreal hi = 0.0;
        qreal mean = 0.0;
        for (const auto &mass : lineMass) {
            lo = std::min(lo, mass[size_t(line)]);
            hi = std::max(hi, mass[size_t(line)]);
            mean += mass[size_t(line)] / kShifts;
        }
        if (mean < 0.5 * peak)
            continue;
        result.massBreath = std::max(result.massBreath, (hi - lo) / mean);
    }
    return result;
}

} // namespace

class TestInkCoverage : public QObject
{
    Q_OBJECT

private slots:
    // Покрытие пиксельным боксом: cov = clamp(min(|d|+hw, .5) − max(|d|−hw, −.5), 0, 1).
    void boxCoverageAcrossTheEdge()
    {
        QCOMPARE(ink::coverage(0.0, 1.0), 1.0);
        QCOMPARE(ink::coverage(1.0, 1.0), 0.5);
        QCOMPARE(ink::coverage(-1.0, 1.0), 0.5);
        QCOMPARE(ink::coverage(1.5, 1.0), 0.0);
        // Тонкая линия - трапеция высотой 2·hw.
        QVERIFY(qAbs(ink::coverage(0.0, 0.2) - 0.4) < 1e-12);
        QVERIFY(qAbs(ink::coverage(0.4, 0.2) - 0.3) < 1e-12);
    }

    // Масса чернил на единицу длины равна толщине при любой толщине - без
    // hairline-приёмов (D7).
    void massEqualsWidthAtAnyThickness()
    {
        for (const qreal hw : {0.05, 0.2, 0.5, 1.5, 4.0}) {
            QVERIFY2(qAbs(massAcross(hw) - 2.0 * hw) < 2e-3,
                     qPrintable(QStringLiteral("hw %1: масса %2").arg(hw).arg(massAcross(hw))));
        }
    }

    // Пол толщины: самое тонкое перо на отдалённом зуме не тоньше 0.3 px.
    void floorKeepsThinnestPenVisible()
    {
        const AxisVertex v = vertex(QPointF(10, 10), QPointF(0, 1), 0.01f, 1.0f);
        QCOMPARE(ink::toDevice(v, 1.0).halfWidth, ink::kDefaultMinHalfWidthPx);
        QVERIFY(qAbs(ink::toDevice(v, 1.0, 0.0).halfWidth - 0.01) < 1e-6);
        QVERIFY(qAbs(ink::toDevice(v, 1.0, 0.3).halfWidth - 0.3) < 1e-9);
        QVERIFY(qAbs(massAcross(ink::toDevice(v, 1.0).halfWidth) - 0.3) < 2e-3);
    }

    // Вершина кромки выносится на 1 device px за кромку: pos = c + n·(hwPx + 1).
    void edgeVertexReachesOnePixelPastTheEdge()
    {
        const AxisVertex v = vertex(QPointF(4, 6), QPointF(1, 0), 1.0f, 1.0f);
        const ink::DeviceVertex d = ink::toDevice(v, 2.0); // hwPx 2, вынос 3 px
        QVERIFY(qAbs(d.position.x() - 5.5) < 1e-9 && qAbs(d.position.y() - 6.0) < 1e-9);
        QVERIFY(qAbs(d.distance - 3.0) < 1e-9);
        QVERIFY(qAbs(d.halfWidth - 2.0) < 1e-9);
    }

    // Внутренняя вершина стыка выносится так же далеко, как вершина кромки
    // (предела по кривизне нет, D7); d - её расстояние до прямых сегментов,
    // s·вынос.
    void innerVertexCarriesScaledDistance()
    {
        const AxisVertex v = vertex(QPointF(0, 0), QPointF(0, 1), 2.0f, 0.8f);
        const ink::DeviceVertex d = ink::toDevice(v, 2.0); // hwPx 4, вынос 5 px
        QVERIFY(qAbs(d.position.x()) < 1e-9 && qAbs(d.position.y() - 2.5) < 1e-6);
        QVERIFY(qAbs(d.distance - 0.8 * 5.0) < 1e-6);
        QVERIFY(qAbs(d.halfWidth - 4.0) < 1e-9);
    }

    // Сценарий «Медленная прокрутка»: кромка не мерцает от субпиксельного
    // сдвига. Штрих растеризуется CPU-зеркалом шейдеров (tests/support/
    // inkraster.h) со сдвигом страницы на 0, 0.25, 0.5 и 0.75 device px - по
    // вертикали и по горизонтали.
    void slowScrollDoesNotFlicker_data()
    {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<qreal>("penWidth");
        QTest::addColumn<qreal>("pxPerUnit");
        // dpr 1.75 и масштаб 100% - устройство; пологая линия - худший
        // случай для вертикальной прокрутки: кромка почти параллельна строкам.
        QTest::newRow("line 1pt dpr1.75") << QStringLiteral("line") << 1.0 << 1.75;
        QTest::newRow("line 0.4pt dpr1.75") << QStringLiteral("line") << 0.4 << 1.75;
        QTest::newRow("line 2.6pt dpr1.25") << QStringLiteral("line") << 2.6 << 1.25;
        QTest::newRow("circle 1pt dpr1.75") << QStringLiteral("circle") << 1.0 << 1.75;
    }

    void slowScrollDoesNotFlicker()
    {
        QFETCH(QString, shape);
        QFETCH(qreal, penWidth);
        QFETCH(qreal, pxPerUnit);

        QList<InkPoint> points;
        if (shape == QStringLiteral("line")) {
            for (int i = 0; i <= 100; ++i)
                points.append({QPointF(10 + i * 1.6, 10 + i * 0.2), 1.0, i / 200.0});
        } else {
            for (int i = 0; i <= 180; ++i) {
                const qreal t = 2.0 * M_PI * i / 180.0;
                points.append({QPointF(40 + 25 * std::cos(t), 40 + 25 * std::sin(t)), 1.0,
                               i / 200.0});
            }
        }
        StrokeTessellator tessellator;
        tessellator.setWidth(penWidth);
        tessellator.setMinWidthFactor(0.0);
        const QVector<AxisVertex> strip = tessellator.tessellate(points);

        Scene scene;
        for (const InkPoint &p : points)
            scene.axis.push_back(p.position * pxPerUnit);
        scene.halfWidthPx = std::max(0.5 * penWidth * pxPerUnit, ink::kDefaultMinHalfWidthPx);
        scene.width = int(std::ceil(200 * pxPerUnit));
        scene.height = int(std::ceil(80 * pxPerUnit));

        const auto material = [&](const QPointF &shift) {
            QTransform pageToDevice = QTransform::fromScale(pxPerUnit, pxPerUnit);
            pageToDevice *= QTransform::fromTranslate(shift.x(), shift.y());
            return inkraster::rasterize(strip, pageToDevice, scene.width, scene.height).alpha;
        };
        const auto msaa4 = [&](const QPointF &shift) {
            return shapeCoverage(scene, shift, msaa4Samples());
        };

        for (const QPointF &direction : {QPointF(0, 1), QPointF(1, 0)}) {
            const Flicker ink = measureFlicker(scene, direction, material);
            const Flicker old = measureFlicker(scene, direction, msaa4);
            qInfo("%s, сдвиг по %s: мерцание %.2f (MSAA 4x %.2f), размах массы %.4f (MSAA 4x %.4f)",
                  QTest::currentDataTag(), direction.y() != 0 ? "y" : "x", ink.flicker,
                  old.flicker, ink.massBreath, old.massBreath);
            QVERIFY2(ink.flicker <= kMaxFlicker,
                     qPrintable(QStringLiteral("мерцание %1 из 255").arg(ink.flicker)));
            QVERIFY2(ink.massBreath <= kMaxMassBreath,
                     qPrintable(QStringLiteral("масса строки гуляет на %1").arg(ink.massBreath)));
            // Контроль: та же мера у прежней отрисовки заметно больше порога,
            // значит, порог что-то отличает.
            QVERIFY(old.flicker >= 10.0);
        }
    }

    void centerVertexStaysOnAxis()
    {
        const AxisVertex v = vertex(QPointF(3, 4), QPointF(0, 0), 1.0f, 0.0f);
        const ink::DeviceVertex d = ink::toDevice(v, 3.0);
        QCOMPARE(d.position, QPointF(3, 4));
        QCOMPARE(d.distance, 0.0);
    }
};

QTEST_APPLESS_MAIN(TestInkCoverage)
#include "tst_inkcoverage.moc"
