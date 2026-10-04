// ink-beauty-pass, D7: покрытие кромки по формулам материала чернил на CPU -
// зеркало шейдеров src/canvas/shaders/ink.{vert,frag} (src/ink/inkcoverage.h).
// Пол толщины - параметр материала, а не геометрии: та же вершина даёт разную
// ширину при разном поле.

#include <QTest>

#include "ink/inkcoverage.h"

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
