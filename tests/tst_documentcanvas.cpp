// Задачи 3.4, 3.5, 3.6, 4.2: узел страницы с заполнителем, уровень детализации,
// вытеснение текстур, зум и панорама. Сценарии из specs/pdf-document/spec.md и
// specs/canvas-view/spec.md.
//
// Тесты работают без окна: холст считает раскладку, зону удержания и запросы
// рендера сам, а граф сцены здесь не нужен - проверяется наблюдаемое состояние.

#include <QSignalSpy>
#include <QTest>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "pdf/pdfdocumentmodel.h"

class TestDocumentCanvas : public QObject
{
    Q_OBJECT

private:
    // Ждёт, пока все видимые страницы получат текстуру.
    static bool waitForTextures(DocumentCanvas *canvas, int timeoutMs = 15000)
    {
        QElapsedTimer timer;
        timer.start();
        while (canvas->placeholderPageCount() > 0 && timer.elapsed() < timeoutMs)
            QTest::qWait(20);
        return canvas->placeholderPageCount() == 0;
    }

    // Ждёт не просто текстур, а устоявшихся: во время движения страницы
    // растеризуются в уменьшенном разрешении, и резкая версия приходит только
    // после того, как вид устаканился.
    static bool waitForSettledTextures(DocumentCanvas *canvas, int timeoutMs = 15000)
    {
        if (!waitForTextures(canvas, timeoutMs))
            return false;
        QTest::qWait(200);
        return waitForTextures(canvas, timeoutMs);
    }

private slots:
    // Сценарий из canvas-view: ещё не отрисованные страницы показываются
    // заполнителями, а не пустым местом.
    void showsPlaceholdersBeforeTexturesArrive()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));

        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        QVERIFY(canvas.visiblePageCount() > 0);
        // Сразу после открытия текстур ещё нет - у каждой видимой страницы
        // есть узел с заполнителем.
        QCOMPARE(canvas.placeholderPageCount(), canvas.visiblePageCount());
        QCOMPARE(canvas.liveTextureCount(), 0);

        QVERIFY(waitForTextures(&canvas));
        QCOMPARE(canvas.placeholderPageCount(), 0);
        QVERIFY(canvas.liveTextureCount() >= canvas.visiblePageCount());
    }

    // Требование «Непрерывное масштабирование»: любые промежуточные значения.
    void zoomAcceptsIntermediateValues()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        for (qreal target : {0.37, 0.913, 1.0, 1.77, 3.14159, 6.5}) {
            canvas.setZoom(target);
            QVERIFY2(qAbs(canvas.zoom() - target) < 1e-6,
                     qPrintable(QStringLiteral("zoom %1 -> %2").arg(target).arg(canvas.zoom())));
        }
    }

    // Сценарий «Границы диапазона»: масштаб встаёт на границе, позиция не
    // теряется и рывка нет.
    void zoomClampsToRangeWithoutLosingPosition()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        QCOMPARE(canvas.minZoom(), 0.25);
        QCOMPARE(canvas.maxZoom(), 8.0);

        canvas.setZoom(100.0);
        QCOMPARE(canvas.zoom(), 8.0);
        const QPointF atMax = canvas.viewToDocument(QPointF(450, 350));

        canvas.setZoom(200.0); // ещё дальше за границу
        QCOMPARE(canvas.zoom(), 8.0);
        const QPointF stillAtMax = canvas.viewToDocument(QPointF(450, 350));
        QVERIFY(qAbs(atMax.x() - stillAtMax.x()) < 1e-6);
        QVERIFY(qAbs(atMax.y() - stillAtMax.y()) < 1e-6);

        canvas.setZoom(0.001);
        QCOMPARE(canvas.zoom(), 0.25);
    }

    // Требование pinch: точка документа под центром жеста остаётся под ним.
    void zoomAtKeepsAnchorPointFixed()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        const QPointF anchor(320, 240);
        const QPointF before = canvas.viewToDocument(anchor);

        canvas.zoomBy(anchor, 2.5);
        const QPointF after = canvas.viewToDocument(anchor);

        QVERIFY2(qAbs(before.x() - after.x()) < 0.5,
                 qPrintable(QStringLiteral("x %1 -> %2").arg(before.x()).arg(after.x())));
        QVERIFY2(qAbs(before.y() - after.y()) < 0.5,
                 qPrintable(QStringLiteral("y %1 -> %2").arg(before.y()).arg(after.y())));
    }

    void panMovesViewAndStaysWithinDocument()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        const QPointF before = canvas.visibleDocumentRect().topLeft();
        canvas.panBy(QPointF(0, -400));
        const QPointF after = canvas.visibleDocumentRect().topLeft();
        QVERIFY(after.y() > before.y());

        // Далеко вверх за начало документа уехать нельзя.
        canvas.panBy(QPointF(0, 1e6));
        QVERIFY(canvas.visibleDocumentRect().top() <= 0.5);
        // И за конец тоже.
        canvas.panBy(QPointF(0, -1e7));
        const qreal documentHeight = canvas.layout().documentSize().height();
        QVERIFY(canvas.visibleDocumentRect().bottom() <= documentHeight + 0.5);
    }

    // Сценарий «Промежуточное состояние при быстром зуме»: пока перерендер не
    // готов, показывается уже отрисованная текстура, а не пустая область.
    void fastZoomKeepsExistingTextureUntilSharpOneArrives()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        QVERIFY(waitForTextures(&canvas));

        const QSize coarse = canvas.textureSizeOf(0);
        QVERIFY(!coarse.isEmpty());

        // Быстрая серия изменений масштаба - как при непрерывном жесте.
        for (int step = 0; step < 20; ++step) {
            canvas.zoomBy(QPointF(450, 350), 1.08);
            // Ни на одном промежуточном шаге страница не остаётся без растра.
            QCOMPARE(canvas.placeholderPageCount(), 0);
        }

        // После остановки зума приходит текстура под новый масштаб.
        QElapsedTimer timer;
        timer.start();
        while (canvas.textureSizeOf(0).width() <= coarse.width() && timer.elapsed() < 15000)
            QTest::qWait(20);

        const QSize sharp = canvas.textureSizeOf(0);
        QVERIFY2(sharp.width() > coarse.width(),
                 qPrintable(QStringLiteral("coarse %1 -> sharp %2")
                                .arg(coarse.width())
                                .arg(sharp.width())));
        QCOMPARE(canvas.placeholderPageCount(), 0);
    }

    // Учёт памяти под растрами: считаются и текстуры, и ещё не сданные в них
    // растры. Тесты идут без окна, поэтому растр остаётся в записи рядом с
    // размером текстуры - отсюда верхняя граница вдвое от одних текстур.
    void countsMemoryOfRetainedRasters()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));

        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        QCOMPARE(canvas.retainedRasterBytes(), 0);

        QVERIFY(waitForTextures(&canvas));

        qint64 textures = 0;
        for (int page = 0; page < model.pageCount(); ++page) {
            const QSize size = canvas.textureSizeOf(page);
            textures += qint64(size.width()) * size.height() * 4;
        }
        QVERIFY(textures > 0);

        const qint64 bytes = canvas.retainedRasterBytes();
        QVERIFY2(bytes >= textures,
                 qPrintable(QStringLiteral("bytes %1 < textures %2").arg(bytes).arg(textures)));
        QVERIFY2(bytes <= 2 * textures,
                 qPrintable(QStringLiteral("bytes %1 > 2x textures %2").arg(bytes).arg(textures)));
    }

    // Требование «Потолок памяти на удерживаемые страницы»: при исчерпанном
    // бюджете вытесняются дальние от кадра страницы, а видимые остаются с
    // растром. Бюджет здесь занижен: проверять вытеснение на настоящих 600 МБ
    // значило бы их занять.
    void evictsFarthestPagesWhenBudgetIsExhausted()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));

        const auto panAcross = [](DocumentCanvas *canvas, int steps) {
            for (int step = 0; step < steps; ++step) {
                canvas->panBy(QPointF(0, -900));
                waitForSettledTextures(canvas, 5000);
            }
        };

        // Сколько страниц держится без ограничения по памяти - это ширина зоны
        // удержания, с которой и сравнивается работа бюджета.
        DocumentCanvas roomy;
        roomy.setSize(QSizeF(900, 700));
        roomy.setDocument(&model);
        roomy.setZoom(0.8);
        QVERIFY(waitForSettledTextures(&roomy));
        QVERIFY(roomy.retainedPageCount() > 0);
        const qint64 perPage = roomy.retainedRasterBytes() / roomy.retainedPageCount();
        QVERIFY(perPage > 0);
        panAcross(&roomy, 12);
        const int retainedWithoutBudget = roomy.retainedPageCount();

        DocumentCanvas tight;
        tight.setSize(QSizeF(900, 700));
        tight.setDocument(&model);
        tight.setZoom(0.8);
        QVERIFY(waitForSettledTextures(&tight));
        // Бюджет на шесть страниц: вся зона удержания в него не влезает, а
        // видимые страницы с ближайшими соседями - влезают.
        const qint64 budget = perPage * 6;
        tight.setRasterMemoryBudget(budget);

        for (int step = 0; step < 12; ++step) {
            tight.panBy(QPointF(0, -900));
            QVERIFY(waitForSettledTextures(&tight, 5000));
            QVERIFY2(tight.retainedRasterBytes() <= budget,
                     qPrintable(QStringLiteral("bytes %1 > budget %2, retained %3")
                                    .arg(tight.retainedRasterBytes())
                                    .arg(budget)
                                    .arg(tight.retainedPageCount())));
            // Вытеснение не задевает видимое.
            QCOMPARE(tight.placeholderPageCount(), 0);
        }

        QVERIFY2(tight.retainedPageCount() < retainedWithoutBudget,
                 qPrintable(QStringLiteral("retained %1 with budget, %2 without")
                                .arg(tight.retainedPageCount())
                                .arg(retainedWithoutBudget)));
    }

    // Задача 3.6: на документе в 200 страниц удерживаемые растры выходят на
    // плато при прокрутке, а не растут линейно.
    void retainedRastersPlateauWhileScrollingLargeDocument()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        QCOMPARE(model.pageCount(), 200);

        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(0.8);

        qint64 peakBytes = 0;
        QSet<int> everSeen;
        for (int step = 0; step < 40; ++step) {
            canvas.panBy(QPointF(0, -2100));
            // Ждём, пока рендер догонит прокрутку: иначе под нагрузкой тест
            // считал бы не вытеснение, а скорость машины.
            waitForTextures(&canvas, 5000);
            peakBytes = qMax(peakBytes, canvas.retainedRasterBytes());
            for (int page = 0; page < model.pageCount(); ++page) {
                if (!canvas.textureSizeOf(page).isEmpty())
                    everSeen.insert(page);
            }
        }

        // Прокрутили насквозь: страниц повидали много...
        QVERIFY2(everSeen.size() > 20,
                 qPrintable(QStringLiteral("only %1 pages rendered").arg(everSeen.size())));
        // ...но память под растрами не росла вместе с ними: она ограничена
        // бюджетом, а на деле - шириной зоны удержания, которая куда уже.
        const qint64 budget = canvas.rasterMemoryBudget();
        QVERIFY2(peakBytes <= budget,
                 qPrintable(QStringLiteral("peak %1 > budget %2").arg(peakBytes).arg(budget)));
        // Зона удержания на этом масштабе - полтора десятка страниц, и это на
        // порядок меньше бюджета: плато определяется зоной, а не потолком.
        const qint64 plateau = canvas.retainedRasterBytes();
        QVERIFY2(plateau * 8 < budget,
                 qPrintable(QStringLiteral("plateau %1 too close to budget %2")
                                .arg(plateau).arg(budget)));
    }

    // Смена режима не теряет позицию чтения.
    void switchingPageModeKeepsCurrentPage()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        canvas.goToPage(7);
        QCOMPARE(canvas.currentPage(), 7);
        const qreal zoom = canvas.zoom();

        canvas.setPageMode(DocumentCanvas::PageMode::Spread);
        QCOMPARE(canvas.pageMode(), DocumentCanvas::PageMode::Spread);
        QCOMPARE(canvas.zoom(), zoom);
        // Страница 7 в развороте лежит в паре (7, 8) - она и остаётся текущей.
        QVERIFY(canvas.layout().pagesInRow(canvas.layout().rowOf(7)).contains(7));
        QCOMPARE(canvas.currentPage(), 7);

        canvas.setPageMode(DocumentCanvas::PageMode::Single);
        QCOMPARE(canvas.currentPage(), 7);
        QCOMPARE(canvas.zoom(), zoom);
    }

    // Сценарий «Завершение жеста»: после броска документ продолжает движение
    // по инерции и плавно останавливается.
    void flickContinuesMotionAndSettles()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        const qreal start = canvas.visibleDocumentRect().top();
        canvas.flick(QPointF(0, -2500)); // бросок вниз по документу
        QVERIFY(canvas.isFlicking());

        // Движение продолжается само, без новых событий. Ждём именно факта
        // сдвига, а не фиксированного времени: под нагрузкой таймер инерции
        // может тикнуть позже, и фиксированная пауза сделала бы тест шатким.
        QElapsedTimer moving;
        moving.start();
        while (canvas.visibleDocumentRect().top() <= start && moving.elapsed() < 2000)
            QTest::qWait(10);
        const qreal midway = canvas.visibleDocumentRect().top();
        QVERIFY2(midway > start, "документ не сдвинулся по инерции");

        // И само же останавливается.
        QElapsedTimer timer;
        timer.start();
        while (canvas.isFlicking() && timer.elapsed() < 5000)
            QTest::qWait(20);
        QVERIFY(!canvas.isFlicking());

        const qreal end = canvas.visibleDocumentRect().top();
        QVERIFY(end >= midway);

        // После остановки позиция больше не меняется.
        QTest::qWait(120);
        QCOMPARE(canvas.visibleDocumentRect().top(), end);
    }

    void slowGestureDoesNotStartFlick()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        canvas.flick(QPointF(0, -5)); // палец почти стоял
        QVERIFY(!canvas.isFlicking());
    }

    void flickStopsAtDocumentEdge()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        canvas.flick(QPointF(0, 9000)); // резко вверх, к началу документа
        QElapsedTimer timer;
        timer.start();
        while (canvas.isFlicking() && timer.elapsed() < 5000)
            QTest::qWait(10);

        QVERIFY(!canvas.isFlicking());
        QVERIFY(canvas.visibleDocumentRect().top() <= 0.5);
    }

    void newGestureCancelsFlick()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        canvas.flick(QPointF(0, -3000));
        QVERIFY(canvas.isFlicking());
        canvas.stopFlick();
        QVERIFY(!canvas.isFlicking());

        const qreal position = canvas.visibleDocumentRect().top();
        QTest::qWait(120);
        QCOMPARE(canvas.visibleDocumentRect().top(), position);
    }

    // Задача 6.2, сценарий «Клик по пункту»: вид перемещается к целевой
    // странице, и она становится текущей.
    void goToPageMovesViewAndMarksCurrentPage()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);

        canvas.goToPage(9);
        QCOMPARE(canvas.currentPage(), 9);
        QVERIFY(canvas.visibleDocumentRect().intersects(canvas.layout().pageRect(9)));
    }

    // Сценарий «Переход в режиме разворота»: показывается разворот с целевой
    // страницей, и целевой считается именно она, а не соседняя по развороту.
    void goToPageInSpreadShowsSpreadWithTargetPage()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(1200, 700));
        canvas.setDocument(&model);
        canvas.setPageMode(DocumentCanvas::PageMode::Spread);

        // Страница 8 - правая в развороте (7, 8).
        const QList<int> row = canvas.layout().pagesInRow(canvas.layout().rowOf(8));
        QCOMPARE(row, QList<int>({7, 8}));

        canvas.goToPage(8);
        QCOMPARE(canvas.currentPage(), 8);

        // Обе страницы разворота попали в кадр.
        const QRectF visible = canvas.visibleDocumentRect();
        QVERIFY(visible.intersects(canvas.layout().pageRect(7)));
        QVERIFY(visible.intersects(canvas.layout().pageRect(8)));
    }

    // Сценарий «Битая ссылка»: переход на несуществующую страницу вид не меняет.
    void goToNonexistentPageLeavesViewUnchanged()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.goToPage(5);

        const QRectF before = canvas.visibleDocumentRect();
        const int page = canvas.currentPage();

        canvas.goToPage(999);
        canvas.goToPage(-1);

        QCOMPARE(canvas.visibleDocumentRect(), before);
        QCOMPARE(canvas.currentPage(), page);
    }

    void emptyDocumentRendersNothing()
    {
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        QCOMPARE(canvas.pageCount(), 0);
        QCOMPARE(canvas.visiblePageCount(), 0);
        QCOMPARE(canvas.placeholderPageCount(), 0);
        QCOMPARE(canvas.liveTextureCount(), 0);
    }
};

QTEST_MAIN(TestDocumentCanvas)
#include "tst_documentcanvas.moc"
