#include <QtTest>

#include "canvas/surfaceformat.h"

// Число сэмплов мультисэмплинга окна (design.md, D8). Кромку чернил
// сглаживает InkMaterial, поэтому по умолчанию MSAA выключен; UQ_MSAA
// возвращает его для сравнения на устройстве.
class TestSurfaceFormat : public QObject
{
    Q_OBJECT

private slots:
    void defaultIsNoMultisampling()
    {
        QCOMPARE(surfaceSampleCount(QByteArray()), 0);
    }

    void environmentRestoresMultisampling_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<int>("expected");
        QTest::newRow("4") << QByteArray("4") << 4;
        QTest::newRow("8") << QByteArray("8") << 8;
        QTest::newRow("0") << QByteArray("0") << 0;
        QTest::newRow("пробелы") << QByteArray(" 4 ") << 4;
    }

    void environmentRestoresMultisampling()
    {
        QFETCH(QByteArray, value);
        QFETCH(int, expected);
        QCOMPARE(surfaceSampleCount(value), expected);
    }

    // Мусор в переменной не должен ни ронять запуск, ни включать странное
    // число сэмплов: тогда действует значение по умолчанию.
    void invalidValueFallsBackToDefault_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("текст") << QByteArray("yes");
        QTest::newRow("отрицательное") << QByteArray("-4");
        QTest::newRow("не степень двойки") << QByteArray("3");
        QTest::newRow("слишком много") << QByteArray("32");
    }

    void invalidValueFallsBackToDefault()
    {
        QFETCH(QByteArray, value);
        QCOMPARE(surfaceSampleCount(value), 0);
    }
};

QTEST_GUILESS_MAIN(TestSurfaceFormat)
#include "tst_surfaceformat.moc"
