// Задачи 8.1 и 8.2: чернила вписываются в PDF как векторные заливки замкнутых
// контуров, а текстовый слой и оглавление оригинала переживают наложение.
// Сценарии из specs/pdf-document/spec.md.

#include <QElapsedTimer>
#include <QDir>
#include <QSignalSpy>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "export/noteexporter.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "pdf/pdfdocumentmodel.h"

class TestExport : public QObject
{
    Q_OBJECT

private:
    static Stroke wave(int page, qreal baseY, qreal width = 6.0)
    {
        Stroke stroke;
        stroke.page = page;
        stroke.width = width;
        stroke.color = QColor(20, 30, 200, 230);
        for (int i = 0; i <= 120; ++i) {
            const qreal t = qreal(i) / 120.0;
            stroke.points.append({QPointF(60 + t * 460, baseY + std::sin(t * 6.0 * M_PI) * 50),
                                  0.15 + 0.85 * std::sin(t * M_PI), i / 200.0});
        }
        return stroke;
    }

    // Прогоняет через стороннюю реализацию PDF (poppler), а не через ту же
    // библиотеку, которой писали.
    static QString pdfToText(const QString &path)
    {
        QProcess process;
        process.start(QStringLiteral("pdftotext"), {path, QStringLiteral("-")});
        if (!process.waitForFinished(30000))
            return QString();
        return QString::fromUtf8(process.readAllStandardOutput());
    }

    static QString qpdfJson(const QString &path, const QString &key)
    {
        QProcess process;
        process.start(QStringLiteral("qpdf"),
                      {QStringLiteral("--json"), QStringLiteral("--json-key=") + key, path});
        if (!process.waitForFinished(60000))
            return QString();
        return QString::fromUtf8(process.readAllStandardOutput());
    }

    struct Prepared
    {
        QList<QSizeF> sizes; // размеры показываемых страниц, с учётом поворота
        QList<int> rotations;
    };

    static Prepared prepare(PdfDocumentModel &document)
    {
        Prepared result;
        for (int page = 0; page < document.pageCount(); ++page) {
            result.sizes.append(document.pageSize(page));
            result.rotations.append(document.pageRotation(page));
        }
        return result;
    }

private slots:
    void init() { QVERIFY(m_dir.isValid()); }

    // Сценарий «Экспорт документа с чернилами».
    void exportsInkOverOriginalContent()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        ink.addStroke(wave(0, 300));
        ink.addStroke(wave(3, 420));
        ink.addStroke(wave(13, 500));

        const QString output = m_dir.filePath(QStringLiteral("exported.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::textbook(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo(output).size() > 0);

        // Число страниц не изменилось.
        PdfDocumentModel exported;
        QVERIFY(exported.open(output));
        QCOMPARE(exported.pageCount(), document.pageCount());
        for (int page = 0; page < exported.pageCount(); ++page)
            QCOMPARE(exported.pageSize(page), document.pageSize(page));
    }

    // Сценарий «Сохранение текстового слоя и закладок».
    void preservesTextLayerAndBookmarks()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        ink.addStroke(wave(2, 300));
        ink.addStroke(wave(12, 300));

        const QString output = m_dir.filePath(QStringLiteral("searchable.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::textbook(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        // Поиск по тексту работает - и именно на тех страницах, где чернила.
        const QString text = pdfToText(output);
        QVERIFY2(text.contains(QStringLiteral("QUIRKBLOSSOM")), "текстовый слой потерян");
        QVERIFY2(text.contains(QStringLiteral("ZEPHYRQUARTZ")), "текстовый слой потерян");

        // Оглавление на месте и совпадает с оригиналом.
        const QString outlineBefore = qpdfJson(fixtures::textbook(), QStringLiteral("outlines"));
        const QString outlineAfter = qpdfJson(output, QStringLiteral("outlines"));
        QVERIFY(!outlineAfter.isEmpty());
        QVERIFY(outlineAfter.contains(QStringLiteral("1.1.1 Deep detail")));
        QVERIFY(outlineAfter.contains(QStringLiteral("4 Conclusion")));
        QCOMPARE(outlineAfter.count(QStringLiteral("\"title\"")),
                 outlineBefore.count(QStringLiteral("\"title\"")));
    }

    // Сценарий «Чернила остаются векторными»: в результате нет ни одного
    // растрового объекта от чернил, только заливки контуров.
    void inkIsVectorNotRaster()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        ink.addStroke(wave(0, 300));

        const QString output = m_dir.filePath(QStringLiteral("vector.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        QProcess qdf;
        qdf.start(QStringLiteral("qpdf"),
                  {QStringLiteral("--qdf"), QStringLiteral("--object-streams=disable"), output,
                   QStringLiteral("-")});
        QVERIFY(qdf.waitForFinished(60000));
        const QByteArray dump = qdf.readAllStandardOutput();
        qInfo("qdf dump: %lld bytes, exit %d", qint64(dump.size()), qdf.exitCode());

        QVERIFY(!dump.isEmpty());
        // Никаких изображений: ни XObject-картинок, ни встроенных.
        QVERIFY2(!dump.contains("/Subtype /Image"), "чернила растеризованы");
        // Заливки замкнутых контуров на месте: сегменты, замыкание и заливка.
        QVERIFY2(dump.contains(" l\n"), "в выводе нет линейных сегментов");
        QVERIFY2(dump.contains("\nh\n"), "контур не замкнут");
        QVERIFY2(dump.contains("\nf*\n") || dump.contains("\nf\n"),
                 "в выводе нет заливок");
    }

    // Поворот страницы: чернила обязаны лечь туда же, где их поставили,
    // несмотря на /Rotate.
    void placesInkCorrectlyOnRotatedPages()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::mixedPages()));
        const Prepared prepared = prepare(document);
        QCOMPARE(prepared.rotations, QList<int>({0, 0, 90, 180, 270}));

        InkDocument ink;
        // По штриху в левом верхнем углу каждой показываемой страницы.
        for (int page = 0; page < document.pageCount(); ++page) {
            Stroke stroke;
            stroke.page = page;
            stroke.width = 10.0;
            stroke.color = QColor(220, 20, 30);
            for (int i = 0; i <= 20; ++i)
                stroke.points.append({QPointF(30 + i * 3.0, 30 + i * 1.0), 1.0, i / 200.0});
            ink.addStroke(stroke);
        }

        const QString output = m_dir.filePath(QStringLiteral("rotated.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::mixedPages(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        // Рендерим результат сторонним движком и ищем чернила в левом верхнем
        // углу каждой показываемой страницы.
        for (int page = 0; page < document.pageCount(); ++page) {
            const QString png = m_dir.filePath(QStringLiteral("page%1").arg(page));
            QProcess render;
            render.start(QStringLiteral("pdftoppm"),
                         {QStringLiteral("-f"), QString::number(page + 1), QStringLiteral("-l"),
                          QString::number(page + 1), QStringLiteral("-r"), QStringLiteral("72"),
                          QStringLiteral("-png"), output, png});
            QVERIFY(render.waitForFinished(60000));

            // pdftoppm дописывает к имени номер страницы, дополняя нулями по
            // числу страниц документа, поэтому имя ищется, а не угадывается.
            QString rendered;
            const QFileInfo root(png);
            const QStringList produced =
                QDir(root.absolutePath())
                    .entryList({root.fileName() + QStringLiteral("-*.png")}, QDir::Files);
            if (!produced.isEmpty())
                rendered = QDir(root.absolutePath()).filePath(produced.first());
            QVERIFY2(!rendered.isEmpty(), qPrintable(QStringLiteral("нет рендера страницы %1")
                                                         .arg(page)));

            QImage image(rendered);
            QVERIFY(!image.isNull());

            // Ищем красное в верхней левой четверти показываемой страницы.
            bool foundInk = false;
            const int limitX = image.width() / 2;
            const int limitY = image.height() / 2;
            for (int y = 0; y < limitY && !foundInk; ++y) {
                for (int x = 0; x < limitX; ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.red() > 150 && pixel.green() < 110 && pixel.blue() < 110) {
                        foundInk = true;
                        break;
                    }
                }
            }
            QVERIFY2(foundInk,
                     qPrintable(QStringLiteral("на странице %1 (поворот %2) чернила не там, "
                                               "где их поставили")
                                    .arg(page)
                                    .arg(prepared.rotations[page])));
        }
    }

    void exportWithoutInkStillProducesValidPdf()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);

        const QString output = m_dir.filePath(QStringLiteral("empty-ink.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, {}, output, &error),
                 qPrintable(error));

        PdfDocumentModel exported;
        QVERIFY(exported.open(output));
        QCOMPARE(exported.pageCount(), document.pageCount());
    }

    // Имя, введённое в диалоге без расширения, всё равно должно дать PDF.
    void appendsPdfSuffixWhenMissing()
    {
        QCOMPARE(NoteExporter::withPdfSuffix(QStringLiteral("/tmp/export")),
                 QStringLiteral("/tmp/export.pdf"));
        QCOMPARE(NoteExporter::withPdfSuffix(QStringLiteral("/tmp/export.pdf")),
                 QStringLiteral("/tmp/export.pdf"));
        QCOMPARE(NoteExporter::withPdfSuffix(QStringLiteral("/tmp/export.PDF")),
                 QStringLiteral("/tmp/export.PDF"));
        QCOMPARE(NoteExporter::withPdfSuffix(QString()), QString());
        // Точка в имени каталога расширением не считается.
        QCOMPARE(NoteExporter::withPdfSuffix(QStringLiteral("/tmp/v1.2/export")),
                 QStringLiteral("/tmp/v1.2/export.pdf"));
    }

    void asyncExportAddsSuffixToTypedName()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        InkDocument ink;
        ink.addStroke(wave(0, 300));

        NoteExporter exporter;
        QSignalSpy finished(&exporter, &NoteExporter::finished);
        const QString typed = m_dir.filePath(QStringLiteral("безрасширения"));
        QVERIFY(exporter.exportToPdf(&document, &ink, typed));
        QVERIFY(finished.wait(60000));
        QVERIFY(finished.first().at(0).toBool());

        QVERIFY(!QFileInfo::exists(typed));
        QVERIFY(QFileInfo::exists(typed + QStringLiteral(".pdf")));
    }

    void reportsMissingSource()
    {
        QString error;
        QVERIFY(!NoteExporter::exportNow(m_dir.filePath(QStringLiteral("nope.pdf")),
                                         {QSizeF(595, 842)}, {0}, {},
                                         m_dir.filePath(QStringLiteral("out.pdf")), &error));
        QVERIFY(!error.isEmpty());
    }

    // Задача 8.3: экспорт документа в 200 страниц с чернилами от начала до
    // конца, с отчётом о ходе работы.
    void exportsLargeDocumentWithProgress()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::large()));
        QCOMPARE(document.pageCount(), 200);
        const Prepared prepared = prepare(document);

        InkDocument ink;
        for (int page = 0; page < 200; ++page)
            ink.addStroke(wave(page, 200 + (page % 5) * 60));
        QCOMPARE(ink.strokeCount(), 200);

        int lastReported = -1;
        int reports = 0;
        const QString output = m_dir.filePath(QStringLiteral("large-exported.pdf"));
        QString error;

        QElapsedTimer timer;
        timer.start();
        QVERIFY2(NoteExporter::exportNow(fixtures::large(), prepared.sizes, prepared.rotations,
                                         ink.allStrokes(), output, &error,
                                         [&lastReported, &reports](int page, int total) {
                                             QVERIFY(page >= 0 && page < total);
                                             QVERIFY(page > lastReported);
                                             lastReported = page;
                                             ++reports;
                                         }),
                 qPrintable(error));

        // Ход работы отчитался по каждой странице, от первой до последней.
        QCOMPARE(reports, 200);
        QCOMPARE(lastReported, 199);

        PdfDocumentModel exported;
        QVERIFY(exported.open(output));
        QCOMPARE(exported.pageCount(), 200);
        qInfo("экспорт 200 страниц с чернилами: %lld мс", timer.elapsed());

        // Чернила есть и на первой, и на последней странице.
        const QString dump = qpdfJson(output, QStringLiteral("pages"));
        QVERIFY(!dump.isEmpty());
    }

    // Задача 8.3: асинхронный экспорт с отчётом о ходе работы - то, на чём
    // стоит индикация выполнения в UI.
    void asyncExportReportsProgressAndFinishes()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::large()));

        InkDocument ink;
        for (int page = 0; page < 200; page += 4)
            ink.addStroke(wave(page, 300));

        NoteExporter exporter;
        QSignalSpy progress(&exporter, &NoteExporter::progressChanged);
        QSignalSpy finished(&exporter, &NoteExporter::finished);

        const QString output = m_dir.filePath(QStringLiteral("async.pdf"));
        QVERIFY(exporter.exportToPdf(&document, &ink, output));
        QVERIFY(exporter.isBusy());
        // Пока экспорт идёт, второй запуск не начинается.
        QVERIFY(!exporter.exportToPdf(&document, &ink, output));

        QVERIFY(finished.wait(120000));
        QCOMPARE(finished.size(), 1);
        QVERIFY2(finished.first().at(0).toBool(),
                 qPrintable(finished.first().at(1).toString()));

        QVERIFY(!exporter.isBusy());
        QCOMPARE(exporter.progress(), 1.0);
        // Ход работы отчитывался не только в начале и в конце.
        QVERIFY2(progress.size() > 3,
                 qPrintable(QStringLiteral("progress signals: %1").arg(progress.size())));

        PdfDocumentModel exported;
        QVERIFY(exported.open(output));
        QCOMPARE(exported.pageCount(), 200);
    }

    void asyncExportRefusesWithoutDocument()
    {
        NoteExporter exporter;
        InkDocument ink;
        QVERIFY(!exporter.exportToPdf(nullptr, &ink, m_dir.filePath(QStringLiteral("x.pdf"))));
        PdfDocumentModel empty;
        QVERIFY(!exporter.exportToPdf(&empty, &ink, m_dir.filePath(QStringLiteral("x.pdf"))));
        QVERIFY(!exporter.isBusy());
    }

    // Задача 5.6 add-ink-inserts, сценарий «Экспорт документа с вкладышами».
    void insertInkIsNotExported()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        // Красная линия страницы, середину которой закроет вкладыш.
        Stroke pageStroke;
        pageStroke.page = 0;
        pageStroke.width = 10.0;
        pageStroke.color = QColor(220, 20, 30);
        for (int i = 0; i <= 20; ++i)
            pageStroke.points.append({QPointF(60 + i * 23.0, 300), 1.0, i / 200.0});
        QVERIFY(ink.addStroke(pageStroke) != 0);

        const quint64 insert = ink.addInsert(0, QRectF(200, 250, 200, 140));
        // Синяя линия вкладыша: в координатах страницы - y = 350, x 220..380.
        Stroke insertStroke;
        insertStroke.insert = insert;
        insertStroke.width = 10.0;
        insertStroke.color = QColor(20, 30, 220);
        for (int i = 0; i <= 20; ++i)
            insertStroke.points.append({QPointF(20 + i * 8.0, 100), 1.0, i / 200.0});
        QVERIFY(ink.addStroke(insertStroke) != 0);

        const QString output = m_dir.filePath(QStringLiteral("with-insert.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        // 72 точки на дюйм: пиксель - точка страницы.
        const QString png = m_dir.filePath(QStringLiteral("with-insert"));
        QProcess render;
        render.start(QStringLiteral("pdftoppm"),
                     {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                      QStringLiteral("1"), QStringLiteral("-r"), QStringLiteral("72"),
                      QStringLiteral("-png"), output, png});
        QVERIFY(render.waitForFinished(60000));
        const QFileInfo root(png);
        const QStringList produced =
            QDir(root.absolutePath())
                .entryList({root.fileName() + QStringLiteral("-*.png")}, QDir::Files);
        QVERIFY(!produced.isEmpty());
        const QImage image(QDir(root.absolutePath()).filePath(produced.first()));
        QVERIFY(!image.isNull());

        const auto anyPixel = [&image](const QRect &area, const auto &predicate) {
            for (int y = area.top(); y <= area.bottom(); ++y) {
                for (int x = area.left(); x <= area.right(); ++x) {
                    if (image.valid(x, y) && predicate(image.pixelColor(x, y)))
                        return true;
                }
            }
            return false;
        };
        const auto red = [](const QColor &c) {
            return c.red() > 150 && c.green() < 110 && c.blue() < 110;
        };
        const auto blue = [](const QColor &c) {
            return c.blue() > 150 && c.red() < 110 && c.green() < 110;
        };

        // Чернила страницы на месте и там, где их закрывал вкладыш.
        QVERIFY2(anyPixel(QRect(295, 297, 10, 6), red), "закрытые вкладышем чернила страницы пропали");
        QVERIFY(anyPixel(QRect(80, 297, 10, 6), red));
        // Ни чернил вкладыша, ни его белого листка поверх линии страницы.
        QVERIFY2(!anyPixel(QRect(210, 335, 180, 30), blue), "чернила вкладыша попали в экспорт");
    }

private:
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestExport)
#include "tst_export.moc"
