// Задачи 3.4, 3.5, 3.6, 4.2: узел страницы с заполнителем, уровень детализации,
// вытеснение текстур, зум и панорама. Сценарии из specs/pdf-document/spec.md и
// specs/canvas-view/spec.md.
//
// Тесты работают без окна: холст считает раскладку, зону удержания и запросы
// рендера сам, а граф сцены здесь не нужен - проверяется наблюдаемое состояние.

#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QSignalSpy>
#include <QTest>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
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

    // Тестовый тачпад. Qt держит устройства ввода в глобальном списке, а
    // события ссылаются на них по указателю, поэтому устройство создаётся один
    // раз и живёт до конца процесса - как в tst_canvasink.
    static const QPointingDevice *touchpad()
    {
        static auto *device = new QPointingDevice(
            QStringLiteral("test touchpad"), 4, QInputDevice::DeviceType::TouchPad,
            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 2, 0);
        return device;
    }

    struct GestureStep
    {
        Qt::NativeGestureType type;
        qreal value = 0.0;
        QPointF delta;
    };

    static GestureStep zoomStep(qreal value) { return {Qt::ZoomNativeGesture, value, {}}; }
    static GestureStep panStep(const QPointF &delta) { return {Qt::PanNativeGesture, 0.0, delta}; }

    // Одно событие пинча на тачпаде. Позиция - курсор: за жест она не
    // меняется, так жест приходит на целевом устройстве (spikes/FINDINGS.md).
    static bool sendGestureEvent(DocumentCanvas *canvas, const GestureStep &step,
                                 const QPointF &cursor)
    {
        QNativeGestureEvent event(step.type, touchpad(), 2, cursor, cursor, cursor, step.value,
                                  step.delta);
        event.setAccepted(false);
        QCoreApplication::sendEvent(canvas, &event);
        return event.isAccepted();
    }

    // Жест целиком: Begin -> шаги -> End. Истина, если приняты все события.
    static bool sendGesture(DocumentCanvas *canvas, const QPointF &cursor,
                            const QList<GestureStep> &steps)
    {
        bool accepted = sendGestureEvent(canvas, {Qt::BeginNativeGesture}, cursor);
        for (const GestureStep &step : steps)
            accepted = sendGestureEvent(canvas, step, cursor) && accepted;
        return sendGestureEvent(canvas, {Qt::EndNativeGesture}, cursor) && accepted;
    }

    static QByteArray zoomMessage(qreal actual, qreal expected)
    {
        return QStringLiteral("масштаб %1, ожидался %2")
            .arg(actual, 0, 'g', 12)
            .arg(expected, 0, 'g', 12)
            .toUtf8();
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

    // --- Пинч на тачпаде ---------------------------------------------------
    // Требование «Масштабирование пинчем на тачпаде» из canvas-view. Жест
    // шлётся холсту событиями напрямую, без окна и без тачпада (design.md, D8).

    // Сценарий «Сведение и разведение пальцев на тачпаде»: масштаб идёт вслед
    // за жестом мелкими шагами, а точка документа под курсором стоит на месте.
    void touchpadPinchZoomsContinuouslyAroundCursor()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(2.5);

        const QPointF cursor(320, 240);
        const QPointF anchor = canvas.viewToDocument(cursor);
        // Шаг - квант тачпада на целевом устройстве, 1/256 масштаба жеста.
        // Разведение до 1.5 от исходного, затем сведение до 1.25.
        const qreal quantum = 1.0 / 256.0;
        QList<qreal> steps(128, quantum);
        steps.append(QList<qreal>(64, -quantum));

        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));
        qreal previous = canvas.zoom();
        for (int i = 0; i < steps.size(); ++i) {
            QVERIFY(sendGestureEvent(&canvas, zoomStep(steps[i]), cursor));
            const qreal zoom = canvas.zoom();
            QVERIFY2(steps[i] > 0 ? zoom > previous : zoom < previous,
                     qPrintable(QStringLiteral("шаг %1: масштаб %2 -> %3")
                                    .arg(i)
                                    .arg(previous, 0, 'g', 12)
                                    .arg(zoom, 0, 'g', 12)));
            previous = zoom;

            const QPointF now = canvas.viewToDocument(cursor);
            QVERIFY2(qAbs(now.x() - anchor.x()) < 0.5 && qAbs(now.y() - anchor.y()) < 0.5,
                     qPrintable(QStringLiteral("шаг %1: точка под курсором %2,%3 -> %4,%5")
                                    .arg(i)
                                    .arg(anchor.x())
                                    .arg(anchor.y())
                                    .arg(now.x())
                                    .arg(now.y())));
        }
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
        QVERIFY2(qAbs(canvas.zoom() - 2.5 * 1.25) < 1e-9,
                 zoomMessage(canvas.zoom(), 2.5 * 1.25).constData());
    }

    // Сценарий «Возврат пальцев в исходное положение»: развести до 2x и свести
    // теми же шагами обратно - масштаб ровно исходный. Накопление умножением
    // этот тест валит: произведение (1 + v)(1 - v) меньше единицы.
    void touchpadPinchThereAndBackRestoresZoom()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.3);

        const QPointF cursor(450, 350);
        const qreal start = canvas.zoom();
        // Шаги неравные, как у настоящего жеста, а в сумме - ровно удвоение.
        const QList<qreal> spread = {0.05, 0.1, 0.02, 0.2, 0.13, 0.25, 0.25};

        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));
        for (qreal value : spread)
            QVERIFY(sendGestureEvent(&canvas, zoomStep(value), cursor));
        QVERIFY2(qAbs(canvas.zoom() - 2.0 * start) < 1e-9,
                 zoomMessage(canvas.zoom(), 2.0 * start).constData());

        for (auto it = spread.crbegin(); it != spread.crend(); ++it)
            QVERIFY(sendGestureEvent(&canvas, zoomStep(-*it), cursor));
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));

        QVERIFY2(qAbs(canvas.zoom() - start) < 1e-9, zoomMessage(canvas.zoom(), start).constData());
    }

    // Сценарий «Упор в границу диапазона»: за границей масштаб и документ
    // стоят, а обратное движение пальцев сразу уменьшает масштаб.
    void touchpadPinchAtRangeLimitReversesImmediately()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(6.0);

        const QPointF cursor(450, 350);
        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));
        // 6 * (1 + 7 * 0.05) = 8.1 - уже за границей.
        for (int i = 0; i < 7; ++i)
            QVERIFY(sendGestureEvent(&canvas, zoomStep(0.05), cursor));
        QCOMPARE(canvas.zoom(), canvas.maxZoom());
        const QRectF atLimit = canvas.visibleDocumentRect();

        // Пальцы расходятся дальше: ни масштаб, ни документ не двигаются.
        for (int i = 0; i < 40; ++i) {
            QVERIFY(sendGestureEvent(&canvas, zoomStep(0.05), cursor));
            QCOMPARE(canvas.zoom(), canvas.maxZoom());
            QCOMPARE(canvas.visibleDocumentRect(), atLimit);
        }

        // Первое же обратное приращение уменьшает масштаб - от упора, а не от
        // того, куда пальцы ушли бы без него: 8.0 - 6.0 * 0.05.
        QVERIFY(sendGestureEvent(&canvas, zoomStep(-0.05), cursor));
        QVERIFY2(qAbs(canvas.zoom() - 7.7) < 1e-9, zoomMessage(canvas.zoom(), 7.7).constData());
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
    }

    // Сценарий «Одновременный сдвиг и масштаб на тачпаде»: сдвиг пальцев тянет
    // документ, а масштаб в том же жесте держит точку под курсором.
    void touchpadPanAndZoomInOneGesture()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(2.5);

        const QPointF cursor(320, 240);
        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));

        // Пальцы вверх - документ вверх: 120 пикселей элемента - это 120 / zoom
        // точек документа.
        const QRectF before = canvas.visibleDocumentRect();
        QVERIFY(sendGestureEvent(&canvas, panStep(QPointF(0, -120)), cursor));
        const QRectF panned = canvas.visibleDocumentRect();
        QVERIFY2(qAbs((panned.top() - before.top()) - 120.0 / 2.5) < 1e-6,
                 qPrintable(QStringLiteral("верх %1 -> %2").arg(before.top()).arg(panned.top())));
        QVERIFY(qAbs(panned.left() - before.left()) < 1e-6);

        const QPointF anchor = canvas.viewToDocument(cursor);
        QVERIFY(sendGestureEvent(&canvas, zoomStep(0.2), cursor));
        const QPointF afterZoom = canvas.viewToDocument(cursor);
        QVERIFY2(qAbs(canvas.zoom() - 3.0) < 1e-9, zoomMessage(canvas.zoom(), 3.0).constData());
        QVERIFY2(qAbs(afterZoom.x() - anchor.x()) < 0.5 && qAbs(afterZoom.y() - anchor.y()) < 0.5,
                 qPrintable(QStringLiteral("точка под курсором %1,%2 -> %3,%4")
                                .arg(anchor.x())
                                .arg(anchor.y())
                                .arg(afterZoom.x())
                                .arg(afterZoom.y())));

        // Сдвиг после масштаба тянет документ уже в новом масштабе.
        const QRectF zoomed = canvas.visibleDocumentRect();
        QVERIFY(sendGestureEvent(&canvas, panStep(QPointF(0, -60)), cursor));
        QVERIFY(qAbs((canvas.visibleDocumentRect().top() - zoomed.top()) - 60.0 / 3.0) < 1e-6);
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
    }

    // Сценарий «Поворот пальцев»: поворот принимается, чтобы не достался
    // родителю, но не меняет ни масштаба, ни положения документа.
    void touchpadRotateIsIgnored()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(2.0);

        const QPointF cursor(450, 350);
        const QRectF rect = canvas.visibleDocumentRect();

        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));
        for (int i = 0; i < 20; ++i) {
            QVERIFY2(sendGestureEvent(&canvas, {Qt::RotateNativeGesture, 1.7, {}}, cursor),
                     "поворот не принят и уйдёт родителю");
        }
        QCOMPARE(canvas.zoom(), 2.0);
        QCOMPARE(canvas.visibleDocumentRect(), rect);

        // Вперемешку с масштабом поворот тоже ничего не добавляет.
        QVERIFY(sendGestureEvent(&canvas, {Qt::RotateNativeGesture, 5.0, {}}, cursor));
        QVERIFY(sendGestureEvent(&canvas, zoomStep(0.1), cursor));
        QVERIFY(sendGestureEvent(&canvas, {Qt::RotateNativeGesture, -8.0, {}}, cursor));
        QVERIFY(sendGestureEvent(&canvas, zoomStep(0.1), cursor));
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
        QVERIFY2(qAbs(canvas.zoom() - 2.4) < 1e-9, zoomMessage(canvas.zoom(), 2.4).constData());
    }

    // Сценарий «Завершение жеста на тачпаде»: документ встаёт сразу, без
    // доводки по инерции. Заодно новый жест гасит инерцию сенсорного броска.
    void touchpadGestureEndsWithoutInertia()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::large()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        const QPointF cursor(450, 350);
        canvas.flick(QPointF(0, -3000));
        QVERIFY(canvas.isFlicking());
        QVERIFY(sendGestureEvent(&canvas, {Qt::BeginNativeGesture}, cursor));
        QVERIFY(!canvas.isFlicking());

        // Быстрое сведение со сдвигом.
        for (int i = 0; i < 12; ++i) {
            QVERIFY(sendGestureEvent(&canvas, panStep(QPointF(0, -80)), cursor));
            QVERIFY(sendGestureEvent(&canvas, zoomStep(-0.03), cursor));
        }
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
        QVERIFY(!canvas.isFlicking());

        const qreal zoom = canvas.zoom();
        const QRectF rect = canvas.visibleDocumentRect();
        QTest::qWait(200);
        QVERIFY(!canvas.isFlicking());
        QCOMPARE(canvas.zoom(), zoom);
        QCOMPARE(canvas.visibleDocumentRect(), rect);
    }

    // Сценарий «Прежние способы масштабирования не затронуты»: после пинча
    // масштаб меняется как прежде, а следующий пинч продолжает с того масштаба,
    // на котором остановились, - в том числе когда Begin до холста не дошёл.
    void touchpadPinchContinuesFromZoomSetElsewhere()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setZoom(1.0);

        const QPointF cursor(450, 350);
        QVERIFY(sendGesture(&canvas, cursor, {zoomStep(0.5), zoomStep(0.5)}));
        QVERIFY2(qAbs(canvas.zoom() - 2.0) < 1e-9, zoomMessage(canvas.zoom(), 2.0).constData());

        canvas.zoomBy(cursor, 1.5); // Ctrl + колесо
        QVERIFY2(qAbs(canvas.zoom() - 3.0) < 1e-9, zoomMessage(canvas.zoom(), 3.0).constData());

        QVERIFY(sendGesture(&canvas, cursor, {zoomStep(0.1)}));
        QVERIFY2(qAbs(canvas.zoom() - 3.3) < 1e-9, zoomMessage(canvas.zoom(), 3.3).constData());

        // Жест начался вне холста: Begin сюда не пришёл.
        canvas.setZoom(2.0); // кнопка на панели
        QVERIFY(sendGestureEvent(&canvas, zoomStep(0.5), cursor));
        QVERIFY(sendGestureEvent(&canvas, {Qt::EndNativeGesture}, cursor));
        QVERIFY2(qAbs(canvas.zoom() - 3.0) < 1e-9, zoomMessage(canvas.zoom(), 3.0).constData());
    }

    // Сценарий «Пинч во время штриха пером»: пока перо в контакте, жест
    // принимается, но ничего не двигает, а штрих не рвётся. После штриха тот же
    // жест масштаб меняет.
    void touchpadPinchWaitsForStylusStroke()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        InkDocument ink;
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setInk(&ink);
        canvas.setPageMode(DocumentCanvas::PageMode::Single);
        canvas.goToPage(0);

        const QPointF cursor(450, 350);
        const qreal zoom = canvas.zoom();
        const QRectF rect = canvas.visibleDocumentRect();
        const QList<GestureStep> pinch = {zoomStep(0.5), panStep(QPointF(0, -80)), zoomStep(0.2)};

        // Штрих слева направо, жест приходит на середине.
        QVERIFY(canvas.beginInkAt(QPointF(400, 350), 0.5, false));
        for (int i = 1; i <= 12; ++i)
            canvas.continueInkAt(QPointF(400 + i * 5, 350), 0.5);

        QVERIFY2(sendGesture(&canvas, cursor, pinch), "жест во время штриха должен приниматься");
        QCOMPARE(canvas.zoom(), zoom);
        QCOMPARE(canvas.visibleDocumentRect(), rect);
        QVERIFY2(canvas.isDrawing(), "штрих прервался на жесте");

        for (int i = 13; i <= 24; ++i)
            canvas.continueInkAt(QPointF(400 + i * 5, 350), 0.5);
        canvas.endInk();

        // Штрих один и дошёл дальше того места, где пришёл жест.
        QCOMPARE(ink.strokeCount(), 1);
        const int page = canvas.pageAtView(QPointF(400, 350));
        QVERIFY(page >= 0);
        const Stroke &stroke = ink.strokesOnPage(page).first().stroke;
        QVERIFY(stroke.points.size() > 1);
        const qreal gestureX = canvas.viewToPage(page, QPointF(460, 350)).x();
        QVERIFY2(stroke.points.last().position.x() > gestureX,
                 qPrintable(QStringLiteral("штрих кончился на x=%1, жест пришёл на x=%2")
                                .arg(stroke.points.last().position.x())
                                .arg(gestureX)));

        // Перо поднято - тот же жест масштабирует.
        QVERIFY(sendGesture(&canvas, cursor, pinch));
        QVERIFY2(qAbs(canvas.zoom() - zoom * 1.7) < 1e-9,
                 zoomMessage(canvas.zoom(), zoom * 1.7).constData());
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

    // Задача 4.7 add-ink-inserts: модель вкладышей для QML не дёргается на
    // кадрах зума и панорамы, а смена размера меняет ровно одну строку.
    void insertModelIsQuietDuringZoomAndPan()
    {
        PdfDocumentModel model;
        QVERIFY(model.open(fixtures::textbook()));
        InkDocument ink;
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&model);
        canvas.setInk(&ink);
        canvas.goToPage(0);

        canvas.setTool(DocumentCanvas::Tool::Insert);
        canvas.beginInkAt(QPointF(400, 150), 0.6, false);
        canvas.endInk();
        const quint64 placed = canvas.insertZOrder().constLast();
        ink.addInsert(1, QRectF(20, 20, 100, 80));

        QAbstractItemModel *inserts = canvas.insertModel();
        QCOMPARE(inserts->rowCount(), 2);
        const int idRole = inserts->roleNames().key("insertId");
        const int rectRole = inserts->roleNames().key("docRect");
        int placedRow = -1;
        for (int row = 0; row < inserts->rowCount(); ++row) {
            if (inserts->data(inserts->index(row, 0), idRole).toULongLong() == placed)
                placedRow = row;
        }
        QVERIFY(placedRow >= 0);
        QCOMPARE(inserts->data(inserts->index(placedRow, 0), rectRole).toRectF(),
                 canvas.insertDocumentRect(placed));

        QSignalSpy changed(inserts, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(inserts, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(inserts, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(inserts, &QAbstractItemModel::rowsRemoved);

        for (int step = 0; step < 10; ++step) {
            canvas.zoomAt(QPointF(450, 350), canvas.zoom() * 1.05);
            canvas.panBy(QPointF(3, -4));
        }
        for (int step = 0; step < 10; ++step) {
            canvas.panBy(QPointF(-3, 4));
            canvas.zoomAt(QPointF(450, 350), canvas.zoom() / 1.05);
        }
        QCOMPARE(changed.size(), 0);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(removed.size(), 0);

        canvas.resizeInsert(placed, QPointF(700, 500));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().at(0).value<QModelIndex>().row(), placedRow);
        QCOMPARE(changed.first().at(1).value<QModelIndex>().row(), placedRow);
        QCOMPARE(inserts->data(inserts->index(placedRow, 0), rectRole).toRectF(),
                 canvas.insertDocumentRect(placed));
        QCOMPARE(reset.size(), 0);
    }
};

QTEST_MAIN(TestDocumentCanvas)
#include "tst_documentcanvas.moc"
