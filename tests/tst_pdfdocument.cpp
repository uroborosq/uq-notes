// Задачи 3.1 и 3.2: модель документа и обработка ошибок открытия.
// Сценарии из specs/pdf-document/spec.md.

#include <QPdfDocument>
#include <QTest>

#include "fixtures.h"
#include "pdf/pdfdocumentmodel.h"

class TestPdfDocument : public QObject
{
    Q_OBJECT

private slots:
    // Сценарий «Открытие корректного PDF».
    void opensValidDocument()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        QCOMPARE(model.state(), PdfDocumentModel::State::Ready);
        QCOMPARE(model.lastOpenResult(), PdfDocumentModel::OpenResult::Success);
        QVERIFY(model.errorString().isEmpty());
        QCOMPARE(model.pageCount(), 14);
        QCOMPARE(model.source(), fixtures::textbook());
    }

    // Сценарий «Страницы разного размера и ориентации»: каждая страница
    // сохраняет собственные размеры и поворот.
    void reportsPerPageSizeAndRotation()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::mixedPages()));
        QCOMPARE(model.pageCount(), 5);

        struct Expected
        {
            QSizeF displayed;
            int rotation;
        };
        // Размер отдаётся уже с применённым поворотом - это то, что страница
        // занимает на экране; сам поворот доступен отдельно.
        const QList<Expected> expected = {
            {QSizeF(595, 842), 0},   // A4 портрет
            {QSizeF(842, 595), 0},   // A4 альбом
            {QSizeF(595, 420), 90},  // A5 портрет, повёрнут на 90
            {QSizeF(612, 792), 180}, // Letter, повёрнут на 180
            {QSizeF(1191, 842), 270} // A3 портрет, повёрнут на 270
        };

        for (int page = 0; page < expected.size(); ++page) {
            const QSizeF size = model.pageSize(page);
            QVERIFY2(qAbs(size.width() - expected[page].displayed.width()) < 1.0,
                     qPrintable(QStringLiteral("page %1 width %2").arg(page).arg(size.width())));
            QVERIFY2(qAbs(size.height() - expected[page].displayed.height()) < 1.0,
                     qPrintable(QStringLiteral("page %1 height %2").arg(page).arg(size.height())));
            QCOMPARE(model.pageRotation(page), expected[page].rotation);
        }
    }

    void reportsNothingForPagesOutsideDocument()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        QCOMPARE(model.pageSize(-1), QSizeF());
        QCOMPARE(model.pageSize(model.pageCount()), QSizeF());
        QCOMPARE(model.pageRotation(-1), 0);
        QCOMPARE(model.pageRotation(model.pageCount()), 0);
    }

    void preservesPageOrder()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        QCOMPARE(model.pageCount(), 200);
        for (int page = 0; page < model.pageCount(); ++page)
            QVERIFY(!model.pageSize(page).isEmpty());
    }

    // Сценарий «Повреждённый или зашифрованный файл».
    void reportsCorruptFile()
    {
        PdfDocumentModel model;
        QVERIFY(!model.open(fixtures::corrupt()));
        QCOMPARE(model.lastOpenResult(), PdfDocumentModel::OpenResult::InvalidFormat);
        QVERIFY(!model.errorString().isEmpty());
        QCOMPARE(model.state(), PdfDocumentModel::State::Empty);
    }

    void reportsMissingFile()
    {
        PdfDocumentModel model;
        QVERIFY(!model.open(fixtures::path(QStringLiteral("does-not-exist.pdf"))));
        QCOMPARE(model.lastOpenResult(), PdfDocumentModel::OpenResult::FileNotFound);
        QVERIFY(!model.errorString().isEmpty());
    }

    void reportsPasswordRequiredAndOpensWithPassword()
    {
        PdfDocumentModel model;
        QVERIFY(!model.open(fixtures::encrypted()));
        QCOMPARE(model.lastOpenResult(), PdfDocumentModel::OpenResult::PasswordRequired);
        QVERIFY(!model.errorString().isEmpty());

        QVERIFY(model.open(fixtures::encrypted(), fixtures::encryptedPassword()));
        QCOMPARE(model.state(), PdfDocumentModel::State::Ready);
        QCOMPARE(model.pageCount(), 14);
    }

    // «Остаётся в рабочем состоянии»: неудача не выбивает открытый документ.
    void failedOpenKeepsCurrentDocument()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        const int pages = model.pageCount();

        QVERIFY(!model.open(fixtures::corrupt()));
        QCOMPARE(model.state(), PdfDocumentModel::State::Ready);
        QCOMPARE(model.pageCount(), pages);
        QCOMPARE(model.source(), fixtures::textbook());
        QCOMPARE(model.attemptedSource(), fixtures::corrupt());
        QVERIFY(!model.errorString().isEmpty());
        // Документ по-прежнему отвечает на запросы.
        QVERIFY(!model.pageSize(0).isEmpty());
    }

    // Диалоги QtQuick отдают URL, а не путь. Кириллица и пробелы в пути
    // переживают преобразование только через toLocalFile, а не через
    // строковую замену "file://".
    void convertsDialogUrlsToPaths()
    {
        const QString awkward =
            QStringLiteral("/home/uroborosq/Документы/Schritte International/kb.pdf");
        const QUrl url = QUrl::fromLocalFile(awkward);

        // Именно эту форму отдаёт QML при приведении url к строке.
        const QString encoded = url.toString(QUrl::FullyEncoded);
        QVERIFY2(encoded.contains(QStringLiteral("%D0")), qPrintable(encoded));

        // Наивная замена "file://" оставляет путь закодированным - файл по
        // такому имени не найдётся, а экспорт запишется не туда.
        const QString naive = encoded.mid(QStringLiteral("file://").size());
        QVERIFY(naive != awkward);

        QCOMPARE(PdfDocumentModel::toLocalPath(url), awkward);
    }

    void suggestsExportNameNextToSource()
    {
        PdfDocumentModel model;
        QVERIFY(model.suggestedExportUrl().isEmpty());

        QVERIFY(model.open(fixtures::textbook()));
        const QUrl suggested = model.suggestedExportUrl();
        QVERIFY(suggested.isLocalFile());

        const QString path = suggested.toLocalFile();
        QCOMPARE(QFileInfo(path).absolutePath(), QFileInfo(fixtures::textbook()).absolutePath());
        QVERIFY(path.endsWith(QStringLiteral(".pdf")));
        QVERIFY(path.contains(QStringLiteral("textbook")));
        // Не предлагаем перезаписать исходник.
        QVERIFY(path != fixtures::textbook());

        QVERIFY(model.folderUrl().isLocalFile());
        QCOMPARE(model.folderUrl().toLocalFile(),
                 QFileInfo(fixtures::textbook()).absolutePath());
    }

    void closeResetsToEmpty()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        model.close();
        QCOMPARE(model.state(), PdfDocumentModel::State::Empty);
        QCOMPARE(model.pageCount(), 0);
        QVERIFY(model.source().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPdfDocument)
#include "tst_pdfdocument.moc"
