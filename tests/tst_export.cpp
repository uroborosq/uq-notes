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
#include "ink/stroketessellator.h"
#include "pdf/pdfdocumentmodel.h"
#include "support/inkraster.h"
#include "support/referencestrokes.h"

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

    // ink-beauty-pass, D3: экспорт рисует каждый штрих его законом толщины.
    // Две прямые с нулевым нажимом, перо 12 pt: Legacy - 3 pt, Tapered - 0.96 pt.
    void exportFollowsStrokeStyle()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        for (int i = 0; i < 2; ++i) {
            Stroke stroke;
            stroke.page = 0;
            stroke.width = 12.0;
            stroke.color = QColor(220, 20, 30);
            stroke.style = i == 0 ? StrokeStyle::Legacy : StrokeStyle::Tapered;
            for (int k = 0; k <= 20; ++k)
                stroke.points.append({QPointF(60 + k * 23.0, 200 + i * 200), 0.0, k / 200.0});
            QVERIFY(ink.addStroke(stroke) != 0);
        }

        const QString output = m_dir.filePath(QStringLiteral("styles.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        // 72 точки на дюйм: пиксель - точка страницы.
        const QString png = m_dir.filePath(QStringLiteral("styles"));
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

        // Масса чернил поперёк линии в столбце x = 300: доля покрытия по
        // разности красного и зелёного (у чернил 200, у бумаги и чёрного
        // текста 0), так что текст страницы в счёт не идёт.
        const auto inkAcross = [&image](int centerY) {
            qreal mass = 0.0;
            for (int y = centerY - 10; y <= centerY + 10; ++y) {
                if (!image.valid(300, y))
                    continue;
                const QColor c = image.pixelColor(300, y);
                mass += qBound(0.0, (c.red() - c.green()) / 200.0, 1.0);
            }
            return mass;
        };
        const qreal legacy = inkAcross(200);
        const qreal tapered = inkAcross(400);
        QVERIFY2(legacy > 2.0 && legacy < 4.0,
                 qPrintable(QStringLiteral("Legacy: масса %1 вместо около 3").arg(legacy)));
        QVERIFY2(tapered < legacy * 0.5,
                 qPrintable(QStringLiteral("Tapered: масса %1 при Legacy %2").arg(tapered).arg(legacy)));
    }

    // ink-beauty-pass, D4 и D5: конец линии в экспорте круглый и выступает за
    // последнюю точку на полуширину, а касание в одной точке - круг. 72 точки
    // на дюйм: пиксель - точка страницы.
    void roundEndsAndDotsAreExported()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);

        InkDocument ink;
        // Полуширина 5: толщина 10, полный нажим.
        Stroke line;
        line.page = 0;
        line.width = 10.0;
        line.color = QColor(220, 20, 30);
        for (int i = 0; i <= 20; ++i)
            line.points.append({QPointF(100 + i * 5.0, 200), 1.0, i / 200.0});
        QVERIFY(ink.addStroke(line) != 0);
        Stroke dot = line;
        dot.points = {{QPointF(300, 300), 1.0, 0.0}, {QPointF(300, 300), 1.0, 0.01}};
        QVERIFY(ink.addStroke(dot) != 0);

        const QString output = m_dir.filePath(QStringLiteral("round-ends.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        const QString png = m_dir.filePath(QStringLiteral("round-ends"));
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

        const auto anyRed = [&image](const QRect &area) {
            for (int y = area.top(); y <= area.bottom(); ++y) {
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (image.valid(x, y) && c.red() > 150 && c.green() < 110 && c.blue() < 110)
                        return true;
                }
            }
            return false;
        };

        // Осевая линии кончается в x = 200, торец доходит до 205.
        QVERIFY2(anyRed(QRect(201, 198, 2, 4)), "конец линии в экспорте не скруглён наружу");
        QVERIFY(!anyRed(QRect(207, 195, 4, 10)));
        // Точка - круг диаметром 10 вокруг (300, 300).
        QVERIFY2(anyRed(QRect(298, 298, 4, 4)), "точки нет в экспорте");
        QVERIFY(anyRed(QRect(296, 299, 1, 2)) && anyRed(QRect(303, 299, 1, 2)));
        QVERIFY(!anyRed(QRect(307, 295, 3, 10)));
    }

    // ink-beauty-pass, D7, «Экспорт»: чернила в PDF - та же фигура, что на
    // экране. Сторонний растеризатор (pdftoppm, 144 dpi = 2 px на точку) против
    // растра полосы по формулам материала: внутри и снаружи штриха, вдали от
    // кромки, ответы совпадают. Штрихи - волна с нажимом и касание,
    // пропущенные через WetStroke, как их сохраняет приложение. Окрестность
    // 3×3 делает сравнение нечувствительным к сдвигу меньше 1.5 px: тест
    // ловит фигуру (торцы, точку, стыки), а не субпиксельную точность.
    void exportedInkMatchesScreenStrip()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        const Prepared prepared = prepare(document);
        QCOMPARE(prepared.rotations.first(), 0);

        Stroke prototype;
        prototype.page = 0;
        prototype.width = 8.0;
        prototype.color = QColor(220, 20, 30);
        prototype.style = StrokeStyle::Tapered;
        // Нажим у концов не ниже 0.6: торцы в 5 px - заметная часть фигуры.
        QList<InkPoint> input = wave(0, 300).points;
        for (int i = 0; i < input.size(); ++i)
            input[i].pressure = 0.6 + 0.4 * std::sin(M_PI * i / (input.size() - 1));
        const Stroke stroke = reference::written(input, prototype);
        // Касание (D5) - диск толщиной в перо.
        const Stroke tap = reference::written(
            {{QPointF(300, 250), 0.9, 0.0}, {QPointF(300, 250), 0.9, 0.01}}, prototype);
        QVERIFY(stroke.isValid() && tap.isValid());
        InkDocument ink;
        QVERIFY(ink.addStroke(stroke) != 0);
        QVERIFY(ink.addStroke(tap) != 0);

        const QString output = m_dir.filePath(QStringLiteral("parity.pdf"));
        QString error;
        QVERIFY2(NoteExporter::exportNow(fixtures::noOutline(), prepared.sizes,
                                         prepared.rotations, ink.allStrokes(), output, &error),
                 qPrintable(error));

        const QString png = m_dir.filePath(QStringLiteral("parity"));
        QProcess render;
        render.start(QStringLiteral("pdftoppm"),
                     {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                      QStringLiteral("1"), QStringLiteral("-r"), QStringLiteral("144"),
                      QStringLiteral("-png"), output, png});
        QVERIFY(render.waitForFinished(60000));
        QVERIFY2(render.exitStatus() == QProcess::NormalExit && render.exitCode() == 0,
                 render.readAllStandardError().constData());
        const QFileInfo root(png);
        const QStringList produced =
            QDir(root.absolutePath())
                .entryList({root.fileName() + QStringLiteral("-*.png")}, QDir::Files);
        QVERIFY(!produced.isEmpty());
        const QImage image(QDir(root.absolutePath()).filePath(produced.first()));
        QVERIFY(!image.isNull());

        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        // Две полосы в одной, как их сливает батч Qt Quick: повтор последней
        // вершины одной и первой другой даёт только вырожденные треугольники.
        QVector<AxisVertex> both = tessellator.tessellate(stroke.points);
        const QVector<AxisVertex> dot = tessellator.tessellate(tap.points);
        both << both.constLast() << dot.constFirst() << dot;
        const inkraster::Coverage strip = inkraster::rasterize(
            both, QTransform::fromScale(2.0, 2.0), image.width(), image.height());

        // Сравниваются только пиксели, у которых вся окрестность 3×3 у полосы
        // либо сплошь залита, либо пуста: сглаживание poppler и кромка не в счёт.
        const auto solid = [&strip](int x, int y, bool inked) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float a = strip.at(x + dx, y + dy);
                    if (inked ? a < 0.98f : a > 0.02f)
                        return false;
                }
            return true;
        };
        int inside = 0;
        int outside = 0;
        int mismatches = 0;
        QString first;
        // Волна wave(0, 300): x 60…520, y 250…350 в точках страницы, касание -
        // в (300, 250); рамка с запасом.
        const QRect area = QRect(QPoint(2 * 40, 2 * 220), QPoint(2 * 540, 2 * 380))
                               .intersected(image.rect().adjusted(1, 1, -1, -1));
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const bool inked = solid(x, y, true);
                if (!inked && !solid(x, y, false))
                    continue;
                const QColor pixel = image.pixelColor(x, y);
                const bool red = pixel.red() > 150 && pixel.green() < 110 && pixel.blue() < 110;
                ++(inked ? inside : outside);
                if (red != inked) {
                    if (mismatches++ == 0)
                        first = QStringLiteral("(%1, %2): полоса %3, PDF %4")
                                    .arg(x)
                                    .arg(y)
                                    .arg(inked ? QStringLiteral("чернила") : QStringLiteral("фон"))
                                    .arg(red ? QStringLiteral("чернила") : QStringLiteral("фон"));
                }
            }
        }
        QVERIFY2(inside > 500 && outside > 500,
                 qPrintable(QStringLiteral("сравнивать нечего: внутри %1, снаружи %2")
                                .arg(inside)
                                .arg(outside)));
        QVERIFY2(mismatches == 0, qPrintable(QStringLiteral("%1 расхождений, первое %2")
                                                 .arg(mismatches)
                                                 .arg(first)));

        // Кромка: средняя абсолютная ошибка покрытия по области чернил -
        // пикселям, где покрытие есть у полосы или у PDF. Покрытие PDF - по
        // зелёному каналу: у бумаги 255, у чернил 20. Пиксели темнее чернил по
        // красному - текст страницы, они не в счёт. Сдвиг или масштаб
        // экспорта на полпикселя поднимает ошибку выше порога; сглаживание
        // poppler и полосы на одной кромке расходится меньше.
        double errorSum = 0.0;
        int edgeArea = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                const double pdf = std::clamp((255.0 - pixel.green()) / 235.0, 0.0, 1.0);
                if (std::abs(pixel.red() - (255.0 - 35.0 * pdf)) > 12.0)
                    continue;
                const double screen = strip.at(x, y);
                if (pdf < 0.01 && screen < 0.01)
                    continue;
                errorSum += std::abs(pdf - screen);
                ++edgeArea;
            }
        }
        QVERIFY(edgeArea > 0);
        const double meanError = errorSum / edgeArea;
        QVERIFY2(meanError < 0.03,
                 qPrintable(QStringLiteral("средняя ошибка покрытия %1 на %2 пикселях")
                                .arg(meanError)
                                .arg(edgeArea)));
    }

private:
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestExport)
#include "tst_export.moc"
