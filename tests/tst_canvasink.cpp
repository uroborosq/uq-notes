// Задачи 5.1, 5.5, 5.6, 5.7, 5.9: разделение указателей по типу устройства,
// сухая геометрия с вытеснением, координаты страницы, ластик и поведение без
// давления. Сценарии из specs/ink-input/spec.md.

#include <QMouseEvent>
#include <QPointingDevice>
#include <QTest>
#include <QTouchEvent>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "pdf/pdfdocumentmodel.h"

class TestCanvasInk : public QObject
{
    Q_OBJECT

private:
    // Qt держит устройства ввода в глобальном списке, а события ссылаются на
    // них по указателю. Устройство на стеке уничтожается раньше, чем Qt о нём
    // забывает, и это рушит кучу на выходе - поэтому они создаются один раз и
    // живут до конца процесса.
    static const QPointingDevice *touchscreen()
    {
        static auto *device = new QPointingDevice(
            QStringLiteral("test touchscreen"), 1, QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0);
        return device;
    }

    static const QPointingDevice *stylus()
    {
        static auto *device = new QPointingDevice(
            QStringLiteral("test stylus"), 2, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        return device;
    }

    static const QPointingDevice *plainMouse()
    {
        static auto *device = new QPointingDevice(
            QStringLiteral("test mouse"), 3, QInputDevice::DeviceType::Mouse,
            QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position, 1, 3);
        return device;
    }

    // Ведёт «пером» по прямой в координатах элемента.
    static void drawLine(DocumentCanvas *canvas, const QPointF &from, const QPointF &to,
                         qreal pressure = 0.6, bool eraser = false)
    {
        canvas->beginInkAt(from, pressure, eraser);
        for (int i = 1; i <= 24; ++i)
            canvas->continueInkAt(from + (to - from) * (qreal(i) / 24.0), pressure);
        canvas->endInk();
    }

private slots:
    void init()
    {
        QVERIFY(m_document.open(fixtures::textbook()));
        m_ink.clear();
        m_canvas.setSize(QSizeF(900, 700));
        m_canvas.setDocument(&m_document);
        m_canvas.setInk(&m_ink);
        m_canvas.setPageMode(DocumentCanvas::PageMode::Single);
        m_canvas.setTool(DocumentCanvas::Tool::Pen);
    }

    // Требование «Привязка чернил к странице»: хранится страница и координаты
    // в ней, а не то, где это было на экране.
    void storesInkInPageCoordinates()
    {
        m_canvas.goToPage(0);
        const QPointF viewPoint(500, 300);
        const int page = m_canvas.pageAtView(viewPoint);
        QVERIFY(page >= 0);
        const QPointF expected = m_canvas.viewToPage(page, viewPoint);

        drawLine(&m_canvas, viewPoint, viewPoint + QPointF(80, 40));

        QCOMPARE(m_ink.strokeCount(), 1);
        const Stroke &stroke = m_ink.strokesOnPage(page).first().stroke;
        QCOMPARE(stroke.page, page);
        const QPointF stored = stroke.points.first().position;
        QVERIFY2(std::hypot(stored.x() - expected.x(), stored.y() - expected.y()) < 1.0,
                 "штрих записан не в координатах страницы");
    }

    // Сценарий «Смена масштаба»: штрих остаётся на том же месте страницы.
    void inkKeepsPagePositionAcrossZoom()
    {
        m_canvas.setZoom(1.0);
        const QPointF viewPoint(500, 300);
        const int page = m_canvas.pageAtView(viewPoint);
        drawLine(&m_canvas, viewPoint, viewPoint + QPointF(60, 0));

        const QList<InkPoint> before = m_ink.strokesOnPage(page).first().stroke.points;
        const QPointF pageAnchor = before.first().position;

        m_canvas.setZoom(3.0);
        const QList<InkPoint> after = m_ink.strokesOnPage(page).first().stroke.points;
        QCOMPARE(after.first().position, pageAnchor);

        // И на экране он теперь там, куда переехала эта точка страницы.
        const QPointF onScreen =
            m_canvas.documentToView(m_canvas.layout().pageRect(page).topLeft() + pageAnchor);
        QVERIFY(m_canvas.pageAtView(onScreen) == page);
    }

    // Сценарий «Переключение на разворот»: чернила остаются на своих страницах.
    void inkSurvivesPageModeSwitch()
    {
        m_canvas.goToPage(3);
        const QPointF viewPoint(450, 350);
        const int page = m_canvas.pageAtView(viewPoint);
        QVERIFY(page >= 0);
        drawLine(&m_canvas, viewPoint, viewPoint + QPointF(50, 30));
        const QPointF stored = m_ink.strokesOnPage(page).first().stroke.points.first().position;

        m_canvas.setPageMode(DocumentCanvas::PageMode::Spread);
        QCOMPARE(m_ink.strokesOnPage(page).size(), 1);
        QCOMPARE(m_ink.strokesOnPage(page).first().stroke.points.first().position, stored);

        m_canvas.setPageMode(DocumentCanvas::PageMode::Single);
        QCOMPARE(m_ink.strokesOnPage(page).first().stroke.points.first().position, stored);
    }

    // Сценарий «Ладонь на экране»: касания не создают чернил ни при каких
    // условиях - холст их вообще не рассматривает как ввод.
    void touchEventsNeverCreateInk()
    {
        const QEventPoint pressed(1, QEventPoint::State::Pressed, QPointF(450, 350),
                                  QPointF(450, 350));
        QTouchEvent begin(QEvent::TouchBegin, touchscreen(), Qt::NoModifier, {pressed});
        QCoreApplication::sendEvent(&m_canvas, &begin);

        const QEventPoint moved(1, QEventPoint::State::Updated, QPointF(500, 380),
                                QPointF(500, 380));
        QTouchEvent update(QEvent::TouchUpdate, touchscreen(), Qt::NoModifier, {moved});
        QCoreApplication::sendEvent(&m_canvas, &update);

        const QEventPoint released(1, QEventPoint::State::Released, QPointF(500, 380),
                                   QPointF(500, 380));
        QTouchEvent end(QEvent::TouchEnd, touchscreen(), Qt::NoModifier, {released});
        QCoreApplication::sendEvent(&m_canvas, &end);

        QCOMPARE(m_ink.strokeCount(), 0);
        QVERIFY(!m_canvas.isDrawing());
    }

    // Сценарий «Жест во время касания пером»: пока стилус в контакте, холст
    // сообщает об этом, и жестовые обработчики отключаются по этому признаку.
    void stylusContactIsVisibleToGestureHandlers()
    {
        QVERIFY(!m_canvas.isStylusActive());
        m_canvas.beginInkAt(QPointF(450, 350), 0.5, false);
        QVERIFY(m_canvas.isStylusActive());
        QVERIFY(m_canvas.isDrawing());
        m_canvas.continueInkAt(QPointF(470, 360), 0.5);
        QVERIFY(m_canvas.isStylusActive());
        m_canvas.endInk();
        QVERIFY(!m_canvas.isStylusActive());
        QVERIFY(!m_canvas.isDrawing());
    }

    void inkStopsFlickOnContact()
    {
        m_canvas.flick(QPointF(0, -3000));
        QVERIFY(m_canvas.isFlicking());
        m_canvas.beginInkAt(QPointF(450, 350), 0.5, false);
        QVERIFY(!m_canvas.isFlicking());
        m_canvas.endInk();
    }

    // Сценарий «Стирание по касанию».
    void eraserRemovesTouchedStrokeOnly()
    {
        const QPointF a(400, 300);
        const QPointF b(600, 300);
        drawLine(&m_canvas, a, b);
        drawLine(&m_canvas, QPointF(400, 450), QPointF(600, 450));
        QCOMPARE(m_ink.strokeCount(), 2);

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        drawLine(&m_canvas, QPointF(500, 295), QPointF(500, 305));
        QCOMPARE(m_ink.strokeCount(), 1);

        // Одно движение ластика - одна отмена.
        m_ink.undo();
        QCOMPARE(m_ink.strokeCount(), 2);
    }

    // Сценарий «Перевёрнутый конец пера»: ластик включается сам и сам же
    // выключается, выбор пользователя при этом не меняется.
    void invertedPenErasesAndRestoresPreviousTool()
    {
        drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300));
        QCOMPARE(m_ink.strokeCount(), 1);

        m_canvas.setTool(DocumentCanvas::Tool::Pen);
        // Перевёрнутый конец: eraser = true при том же выбранном инструменте.
        drawLine(&m_canvas, QPointF(500, 295), QPointF(500, 305), 0.6, true);

        QCOMPARE(m_ink.strokeCount(), 0);
        // Выбор пользователя не тронут - перо так и осталось выбранным.
        QCOMPARE(m_canvas.tool(), DocumentCanvas::Tool::Pen);

        // И следующий штрих обычным концом снова рисует.
        drawLine(&m_canvas, QPointF(400, 500), QPointF(600, 500));
        QCOMPARE(m_ink.strokeCount(), 1);
    }

    // Сценарий «Перо без данных о давлении»: штрих рисуется, без ошибок,
    // постоянной толщиной.
    void missingPressureStillDraws()
    {
        drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300), -1.0);
        QCOMPARE(m_ink.strokeCount(), 1);

        const Stroke &stroke = m_ink.allStrokes().first().stroke;
        QVERIFY(stroke.points.size() > 2);
        for (const InkPoint &point : stroke.points)
            QVERIFY(!point.hasPressure());
    }

    void tapWithoutMovementLeavesNoStroke()
    {
        m_canvas.beginInkAt(QPointF(450, 350), 0.5, false);
        m_canvas.endInk();
        QCOMPARE(m_ink.strokeCount(), 0);
    }

    void inkInGapBetweenPagesIsIgnored()
    {
        m_canvas.setZoom(0.3); // видно промежуток между страницами
        // Точка выше первой страницы: в раскладке там пусто.
        const QPointF outside(20, 5);
        if (m_canvas.pageAtView(outside) < 0) {
            QVERIFY(!m_canvas.beginInkAt(outside, 0.5, false));
            m_canvas.endInk();
            QCOMPARE(m_ink.strokeCount(), 0);
        }
    }

    void cancelDiscardsStrokeInProgress()
    {
        m_canvas.beginInkAt(QPointF(450, 350), 0.5, false);
        for (int i = 1; i <= 10; ++i)
            m_canvas.continueInkAt(QPointF(450 + i * 5, 350), 0.5);
        m_canvas.cancelInk();
        QCOMPARE(m_ink.strokeCount(), 0);
        QVERIFY(!m_canvas.isStylusActive());
    }

    // Спайк 1.1 показал, что перо на целевом устройстве приходит мышиным
    // событием с устройством Stylus, а не планшетным, и давление при этом
    // приходит настоящее. Разбор указателя вынесен в чистую функцию - её и
    // проверяем: фабриковать события указателя в обход оконной системы Qt
    // ненадёжно.
    void stylusPressureIsTakenFromTheEvent()
    {
        const auto intent = DocumentCanvas::intentFor(stylus(), Qt::LeftButton, 0.63);
        QVERIFY(intent.stylus);
        QVERIFY(intent.contact);
        QVERIFY(!intent.eraser);
        QVERIFY(qAbs(intent.pressure - 0.63) < 1e-9);
    }

    // У настоящей мыши давления нет - штрих идёт постоянной толщиной.
    void mousePressureIsReportedAsUnknown()
    {
        const auto intent = DocumentCanvas::intentFor(plainMouse(), Qt::LeftButton, 1.0);
        QVERIFY(!intent.stylus);
        QVERIFY(intent.contact);
        QVERIFY(intent.pressure < 0.0);
    }

    // У пера целевого устройства нет второго конца, зато есть кнопка на
    // корпусе: спайк 1.1 показал, что она приходит как Qt::MiddleButton.
    void barrelButtonTurnsStylusIntoEraser()
    {
        // Кнопка зажата на весу - ластик уже включён.
        QVERIFY(DocumentCanvas::intentFor(stylus(), Qt::MiddleButton, 0.0).eraser);
        // И во время касания тоже.
        const auto drawing =
            DocumentCanvas::intentFor(stylus(), Qt::LeftButton | Qt::MiddleButton, 0.4);
        QVERIFY(drawing.eraser);
        QVERIFY(drawing.contact);
        // Отпустили кнопку - ластика нет.
        QVERIFY(!DocumentCanvas::intentFor(stylus(), Qt::LeftButton, 0.4).eraser);
    }

    // Перевёрнутый конец пера, если он у пера есть, работает так же.
    void invertedPenEndTurnsStylusIntoEraser()
    {
        static auto *eraserEnd = new QPointingDevice(
            QStringLiteral("test eraser end"), 4, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        QVERIFY(DocumentCanvas::intentFor(eraserEnd, Qt::LeftButton, 0.5).eraser);
    }

    // Средняя кнопка настоящей мыши ластиком не становится: она тянет холст.
    void middleButtonOfRealMouseIsNotEraser()
    {
        QVERIFY(!DocumentCanvas::intentFor(plainMouse(), Qt::MiddleButton, 1.0).eraser);
    }

    void noDeviceIsNotAStylus()
    {
        const auto intent = DocumentCanvas::intentFor(nullptr, Qt::LeftButton, 0.5);
        QVERIFY(!intent.stylus);
        QVERIFY(!intent.eraser);
        QVERIFY(intent.pressure < 0.0);
    }

    // Регрессия: законченный штрих обязан попасть в геометрию страницы сразу,
    // а не при ближайшей панораме или зуме. Ошибка выглядела так: перо пишет,
    // штрих сохраняется, но на экране его нет до первого движения вида.
    void finishedStrokeEntersPageGeometryWithoutViewChange()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);
        QVERIFY(page >= 0);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 0);

        drawLine(&m_canvas, at, at + QPointF(90, 40));

        // Ни панорамы, ни зума между письмом и проверкой.
        QCOMPARE(m_ink.strokeCount(), 1);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);

        drawLine(&m_canvas, at + QPointF(0, 60), at + QPointF(90, 100));
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 2);
    }

    // И стирание тоже видно сразу.
    void erasedStrokeLeavesPageGeometryWithoutViewChange()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);
        drawLine(&m_canvas, at, at + QPointF(90, 0));
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        drawLine(&m_canvas, at + QPointF(45, -5), at + QPointF(45, 5));

        QCOMPARE(m_ink.strokeCount(), 0);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 0);
    }

    // Отмена и повтор тоже обязаны быть видны немедленно.
    void undoAndRedoUpdatePageGeometryImmediately()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);
        drawLine(&m_canvas, at, at + QPointF(90, 0));
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);

        m_ink.undo();
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 0);

        m_ink.redo();
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);
    }

    // Чернила на странице, загруженной из файла, видны без движения вида.
    void loadedInkAppearsOnVisiblePages()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);

        QList<InkDocument::Record> records;
        Stroke stroke;
        stroke.page = page;
        stroke.width = 2.0;
        for (int i = 0; i <= 20; ++i)
            stroke.points.append({QPointF(40 + i * 8.0, 120), 0.5, i / 200.0});
        records.append({1, stroke});

        m_ink.loadStrokes(records, 2);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);
    }

    // Задача 10.3: толщина применяется к новым штрихам и не трогает
    // нанесённые - сценарий «Смена толщины».
    void penWidthAppliesToNewStrokesOnly()
    {
        m_canvas.goToPage(0);
        m_canvas.setPenWidth(0.7);
        drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300));

        m_canvas.setPenWidth(4.5);
        drawLine(&m_canvas, QPointF(400, 400), QPointF(600, 400));

        const QList<InkDocument::Record> all = m_ink.allStrokes();
        QCOMPARE(all.size(), 2);
        QVERIFY(qAbs(all[0].stroke.width - 0.7) < 1e-9);
        QVERIFY(qAbs(all[1].stroke.width - 4.5) < 1e-9);
    }

    // Диапазон должен доходить до волосяной линии: ей и пишут мелкие пометки
    // на увеличенном масштабе.
    void penWidthRangeReachesHairline()
    {
        QVERIFY(DocumentCanvas::minPenWidth() <= 0.5);
        QVERIFY(DocumentCanvas::maxPenWidth() >= 6.0);

        m_canvas.setPenWidth(0.0);
        QCOMPARE(m_canvas.penWidth(), DocumentCanvas::minPenWidth());
        m_canvas.setPenWidth(1000.0);
        QCOMPARE(m_canvas.penWidth(), DocumentCanvas::maxPenWidth());
    }

    // Задача 10.4, сценарий «Выбор цвета».
    void penColourAppliesToNewStrokesOnly()
    {
        m_canvas.goToPage(0);
        m_canvas.setPenColor(QColor("#ffc02828"));
        drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300));

        // Полупрозрачный маркер тоже должен доходить до штриха как есть.
        m_canvas.setPenColor(QColor(0x66, 0xf2, 0xd0, 0x24));
        drawLine(&m_canvas, QPointF(400, 400), QPointF(600, 400));

        const QList<InkDocument::Record> all = m_ink.allStrokes();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].stroke.color, QColor("#ffc02828"));
        QCOMPARE(all[1].stroke.color, QColor(0x66, 0xf2, 0xd0, 0x24));
    }

    // Задача 5.5: геометрия чернил кэшируется по страницам и вытесняется
    // вместе с текстурами.
    void inkGeometryIsCachedAndEvicted()
    {
        PdfDocumentModel large;
        QVERIFY(large.open(fixtures::large()));
        InkDocument ink;
        DocumentCanvas canvas;
        canvas.setSize(QSizeF(900, 700));
        canvas.setDocument(&large);
        canvas.setInk(&ink);

        // Чернила на каждой десятой странице по всему документу.
        for (int page = 0; page < 200; page += 10) {
            Stroke stroke;
            stroke.page = page;
            for (int i = 0; i <= 20; ++i)
                stroke.points.append({QPointF(50 + i * 10, 100), 0.6, i / 200.0});
            ink.addStroke(stroke);
        }
        QCOMPARE(ink.strokeCount(), 20);

        canvas.goToPage(0);
        QVERIFY(canvas.inkGeometryPageCount() <= 3);

        int peak = 0;
        for (int step = 0; step < 100; ++step) {
            canvas.panBy(QPointF(0, -900));
            peak = qMax(peak, canvas.inkGeometryPageCount());
        }

        // Кэш геометрии не растёт вместе с числом страниц с чернилами.
        QVERIFY2(peak <= 6, qPrintable(QStringLiteral("ink geometry peak %1").arg(peak)));
        // А сами штрихи никуда не делись - вытесняется только геометрия.
        QCOMPARE(ink.strokeCount(), 20);
    }

private:
    PdfDocumentModel m_document;
    InkDocument m_ink;
    DocumentCanvas m_canvas;
};

QTEST_MAIN(TestCanvasInk)
#include "tst_canvasink.moc"
