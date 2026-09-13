// Проверка окружения сборки (задачи 2.2, 2.3): тестовая цель линкуется с
// libqpdf и QtPdf и умеет их вызывать, фикстуры на месте.

#include <QFileInfo>
#include <QPdfDocument>
#include <QTest>

#include <qpdf/QPDF.hh>

#include "fixtures.h"

class TestBuildEnv : public QObject
{
    Q_OBJECT

private slots:
    void qpdfLibraryIsLinkedAndCallable()
    {
        const QString version = QString::fromStdString(QPDF::QPDFVersion());
        QVERIFY2(!version.isEmpty(), "libqpdf reported an empty version");
        QVERIFY(version.at(0).isDigit());
        qInfo("libqpdf version: %s", qPrintable(version));
    }

    void qtPdfIsLinkedAndCallable()
    {
        QPdfDocument document;
        QCOMPARE(document.status(), QPdfDocument::Status::Null);
        QCOMPARE(document.pageCount(), 0);
    }

    void fixturesArePresent_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("textbook") << fixtures::textbook();
        QTest::newRow("mixed-pages") << fixtures::mixedPages();
        QTest::newRow("no-outline") << fixtures::noOutline();
        QTest::newRow("large") << fixtures::large();
        QTest::newRow("corrupt") << fixtures::corrupt();
        QTest::newRow("encrypted") << fixtures::encrypted();
    }

    void fixturesArePresent()
    {
        QFETCH(QString, path);
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
    }
};

QTEST_GUILESS_MAIN(TestBuildEnv)
#include "tst_buildenv.moc"
