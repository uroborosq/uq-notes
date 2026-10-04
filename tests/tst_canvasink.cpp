// Задачи 5.1, 5.5, 5.6, 5.7, 5.9: разделение указателей по типу устройства,
// сухая геометрия с вытеснением, координаты страницы, ластик и поведение без
// давления. Сценарии из specs/ink-input/spec.md.

#include <QMouseEvent>
#include <QLineF>
#include <QPointingDevice>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTouchEvent>

#include <cstdint>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "ink/stroketessellator.h"
#include "pdf/pdfdocumentmodel.h"
#include "perf/framestats.h"
#include "storage/notestore.h"

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

    // ink-beauty-pass, D5, сценарий «Точка над буквой»: касание пером без
    // движения оставляет точку - видимую уже во время касания, одним штрихом
    // в истории и в геометрии страницы.
    void tapWithoutMovementLeavesDot()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);
        QVERIFY(page >= 0);

        QVERIFY(m_canvas.beginInkAt(at, 0.5, false));
        // Мокрая точка видна под пером до отрыва: диск из двух полувееров.
        QVERIFY2(m_canvas.wetStripVertexCount()
                     >= StrokeTessellator::kStartCapVertices + StrokeTessellator::kEndCapVertices,
                 qPrintable(QStringLiteral("мокрая полоса: %1 вершин")
                                .arg(m_canvas.wetStripVertexCount())));
        m_canvas.endInk();

        QCOMPARE(m_ink.strokeCount(), 1);
        const Stroke dot = m_ink.allStrokes().first().stroke;
        QCOMPARE(dot.points.size(), 2);
        QCOMPARE(dot.points[0].position, dot.points[1].position);
        // Модель штриха считает во float: точка касания - с точностью до 1e-3.
        QVERIFY(QLineF(dot.points[0].position, m_canvas.viewToPage(page, at)).length() < 1e-3);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);

        // Отмена и повтор - как у любого штриха.
        m_ink.undo();
        QCOMPARE(m_ink.strokeCount(), 0);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 0);
        m_ink.redo();
        QCOMPARE(m_ink.strokeCount(), 1);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), 1);
    }

    // Сценарий «Случайный задев»: касание короче 30 мс с давлением ниже 0.05
    // следа не оставляет. Мышь давления не сообщает - её щелчок ставит точку.
    void accidentalGrazeLeavesNothing()
    {
        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        QVERIFY(m_canvas.beginInkAt(at, 0.03, false, 20.000));
        m_canvas.continueInkAt(at + QPointF(0.2, 0.0), 0.04, 20.010);
        m_canvas.endInk();
        QCOMPARE(m_ink.strokeCount(), 0);
        QVERIFY(!m_canvas.isDrawing());

        QVERIFY(m_canvas.beginInkAt(at, -1.0, false, 21.000));
        m_canvas.endInk();
        QCOMPARE(m_ink.strokeCount(), 1);
    }

    // Сценарий «Точка над буквой»: точка сохраняется в заметке, в контуре
    // экспорта она круг, и ластик стирает её, как любой штрих.
    void dotIsSavedExportedAndErased()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString notePath = dir.filePath(QStringLiteral("dot.uqn"));
        NoteStore store;
        QVERIFY(store.create(notePath, fixtures::textbook()));
        store.attach(&m_ink);

        m_canvas.goToPage(0);
        const QPointF at(450, 350);
        QVERIFY(m_canvas.beginInkAt(at, 0.6, false, 30.0));
        m_canvas.endInk();
        QCOMPARE(m_ink.strokeCount(), 1);
        const Stroke dot = m_ink.allStrokes().first().stroke;

        QVERIFY(store.flush());
        {
            NoteStore reopened;
            InkDocument restored;
            QVERIFY(reopened.openNote(notePath));
            QVERIFY(reopened.load(&restored));
            QCOMPARE(restored.strokeCount(), 1);
            const QList<InkPoint> points = restored.allStrokes().first().stroke.points;
            QCOMPARE(points.size(), 2);
            for (const InkPoint &point : points)
                QVERIFY(QLineF(point.position, dot.points.first().position).length() < 1e-3);
        }

        // Контур экспорта - круг диаметром в толщину точки.
        StrokeTessellator tessellator;
        tessellator.setWidth(dot.width);
        tessellator.setStyle(dot.style);
        const QRectF bounds = tessellator.outline(dot.points).boundingRect();
        const qreal diameter = 2.0 * tessellator.halfWidthAt(dot.points.last());
        QVERIFY(qAbs(bounds.width() - diameter) < 1e-3);
        QVERIFY(qAbs(bounds.height() - diameter) < 1e-3);

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        drawLine(&m_canvas, at + QPointF(-6, 0), at + QPointF(6, 0));
        QCOMPARE(m_ink.strokeCount(), 0);
    }

    // ink-beauty-pass, D3, сценарий «Заострение новых штрихов»: штрих пера
    // получает заострённый закон толщины.
    void penStrokesAreTapered()
    {
        m_canvas.goToPage(0);
        drawLine(&m_canvas, QPointF(400, 300), QPointF(600, 300));
        QCOMPARE(m_ink.strokeCount(), 1);
        QCOMPARE(m_ink.allStrokes().first().stroke.style, StrokeStyle::Tapered);
    }

    // Сухая геометрия рисует каждый штрих его собственным законом: штрихи,
    // нанесённые раньше (Legacy), толщину не меняют, а перо без давления
    // рисует прежнюю толщину и в заострённом штрихе.
    void dryGeometryFollowsStrokeStyle()
    {
        m_canvas.goToPage(0);
        const int page = m_canvas.pageAtView(QPointF(450, 350));
        QVERIFY(page >= 0);

        struct Spec
        {
            StrokeStyle style;
            qreal pressure;
            qreal expectedFactor;
        };
        const QList<Spec> specs = {
            {StrokeStyle::Legacy, 0.0, 0.25},
            {StrokeStyle::Tapered, 0.0, 0.08},
            {StrokeStyle::Tapered, -1.0, 0.7375},
        };
        QList<InkDocument::Record> records;
        for (int i = 0; i < specs.size(); ++i) {
            Stroke stroke;
            stroke.page = page;
            stroke.width = 4.0;
            stroke.style = specs[i].style;
            for (int k = 0; k <= 20; ++k)
                stroke.points.append({QPointF(40 + k * 8.0, 120 + i * 40), specs[i].pressure, k / 200.0});
            records.append({quint64(i + 1), stroke});
        }
        m_ink.loadStrokes(records, specs.size() + 1);
        QCOMPARE(m_canvas.inkGeometryStrokeCount(page), int(specs.size()));

        // Прямая вдоль x с постоянным нажимом: вертикальный размах полосы -
        // полная толщина штриха.
        for (int i = 0; i < specs.size(); ++i) {
            const QVector<StrokeTessellator::Vertex> strip =
                m_canvas.inkGeometryStrip(page, quint64(i + 1));
            QVERIFY2(strip.size() >= 4, qPrintable(QString::number(i)));
            qreal top = 1e9;
            qreal bottom = -1e9;
            for (const StrokeTessellator::Vertex &vertex : strip) {
                const QPointF edge = vertex.at(vertex.hw);
                top = qMin(top, edge.y());
                bottom = qMax(bottom, edge.y());
            }
            const qreal width = bottom - top;
            QVERIFY2(qAbs(width - 4.0 * specs[i].expectedFactor) < 1e-3,
                     qPrintable(QStringLiteral("штрих %1: толщина %2, ожидалась %3")
                                    .arg(i + 1)
                                    .arg(width)
                                    .arg(4.0 * specs[i].expectedFactor)));
        }
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

    // rnote-grade-stroke-fidelity, задача 3.2: время точки берётся из события
    // пера, а не из момента, когда холст успел его обработать.
    void inkPointsTakeTimeFromTheEvent()
    {
        m_canvas.goToPage(0);
        const QPointF from(420, 320);
        const auto send = [this](QEvent::Type type, const QPointF &at, Qt::MouseButton button,
                                 Qt::MouseButtons buttons, quint64 ms) {
            QMouseEvent event(type, at, at, button, buttons, Qt::NoModifier, stylus());
            event.setTimestamp(ms);
            QCoreApplication::sendEvent(&m_canvas, &event);
        };
        const auto pathAt = [from](int i) {
            return from + QPointF(i * 4.0, std::sin(i * 0.3) * 10.0);
        };

        // Метки времени нарочно далеки от часов приложения.
        const quint64 startMs = 5000000;
        const quint64 endMs = startMs + 40 * 5;
        send(QEvent::MouseButtonPress, pathAt(0), Qt::LeftButton, Qt::LeftButton, startMs);
        for (int i = 1; i <= 40; ++i)
            send(QEvent::MouseMove, pathAt(i), Qt::NoButton, Qt::LeftButton, startMs + i * 5);
        send(QEvent::MouseButtonRelease, pathAt(40), Qt::LeftButton, Qt::NoButton, endMs);

        QCOMPARE(m_ink.strokeCount(), 1);
        const QList<InkPoint> points = m_ink.allStrokes().first().stroke.points;
        QVERIFY(points.size() > 2);
        for (const InkPoint &point : points) {
            QVERIFY2(point.timeSeconds >= startMs / 1000.0 - 1e-6
                         && point.timeSeconds <= endMs / 1000.0 + 1e-6,
                     qPrintable(QStringLiteral("время точки %1 не из событий [%2, %3]")
                                    .arg(point.timeSeconds, 0, 'f', 4)
                                    .arg(startMs / 1000.0, 0, 'f', 3)
                                    .arg(endMs / 1000.0, 0, 'f', 3)));
        }
    }

    // ink-beauty-pass, D1: латентность пера отсчитывается от метки события, а
    // не от момента, когда холст его обработал. Метка не из CLOCK_MONOTONIC
    // не теряется молча, а видна в сводке кадров.
    void inputLatencyStartsAtEventStamp()
    {
        FrameStats stats;
        m_canvas.setFrameStats(&stats);
        const auto restore = qScopeGuard([this] {
            m_canvas.cancelInk();
            m_canvas.setFrameStats(nullptr);
        });
        m_canvas.goToPage(0);
        const QPointF from(420, 320);
        const auto send = [this](QEvent::Type type, const QPointF &at, Qt::MouseButton button,
                                 Qt::MouseButtons buttons, quint64 ms) {
            QMouseEvent event(type, at, at, button, buttons, Qt::NoModifier, stylus());
            event.setTimestamp(ms);
            QCoreApplication::sendEvent(&m_canvas, &event);
        };
        // Так метку ставит композитор: миллисекунды CLOCK_MONOTONIC, 32 бита.
        const auto monotonicMs = [] { return quint64(uint32_t(FrameStats::nowNs() / 1'000'000)); };

        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, monotonicMs());
        send(QEvent::MouseMove, from + QPointF(10, 0), Qt::NoButton, Qt::LeftButton, monotonicMs());
        QCOMPARE(stats.foreignInputStamps(), 0);

        // Метка минутной давности - не та шкала часов.
        send(QEvent::MouseMove, from + QPointF(20, 0), Qt::NoButton, Qt::LeftButton,
             monotonicMs() - 60'000);
        QCOMPARE(stats.foreignInputStamps(), 1);
        send(QEvent::MouseButtonRelease, from + QPointF(20, 0), Qt::LeftButton, Qt::NoButton,
             monotonicMs());
    }

    // Сценарий «Неравномерная доставка событий»: те же отсчёты с теми же
    // метками времени, поданные равномерно и пачками, дают один и тот же штрих.
    void burstDeliveryGivesTheSameStroke()
    {
        m_canvas.goToPage(0);

        struct Sample
        {
            QPointF at;
            qreal pressure = 0.0;
            qreal time = 0.0;
        };
        // Метки с разрешением в 1 мс, как на Wayland: при отсчётах через 0.8 мс
        // у соседних метка местами совпадает. И один отсчёт повторён целиком.
        QList<Sample> samples;
        for (int i = 0; i < 240; ++i) {
            samples.append({QPointF(380 + i * 0.5, 330 + 25 * std::sin(i * 0.05)),
                            0.3 + 0.5 * std::abs(std::sin(i * 0.02)),
                            7.0 + std::floor(i * 0.8) / 1000.0});
        }
        samples.insert(51, samples[50]);

        const auto draw = [&](const QList<int> &bursts) {
            m_ink.clear();
            m_canvas.beginInkAt(samples[0].at, samples[0].pressure, false, samples[0].time);
            int burst = 0;
            int inBurst = 0;
            for (int i = 1; i < samples.size(); ++i) {
                m_canvas.continueInkAt(samples[i].at, samples[i].pressure, samples[i].time);
                // Между пачками приложение занято чем-то другим.
                if (++inBurst == bursts[burst % bursts.size()]) {
                    QTest::qWait(2);
                    inBurst = 0;
                    ++burst;
                }
            }
            m_canvas.endInk();
            return m_ink.allStrokes().first().stroke.points;
        };

        const QList<InkPoint> uniform = draw({1});
        const QList<InkPoint> bursty = draw({4, 7, 5, 8, 6});

        QCOMPARE(bursty.size(), uniform.size());
        for (int i = 0; i < uniform.size(); ++i) {
            QVERIFY2(uniform[i].position == bursty[i].position
                         && uniform[i].pressure == bursty[i].pressure,
                     qPrintable(QStringLiteral("точка %1 разошлась").arg(i)));
        }
    }

    // Задача 5.4, сценарий «Нанесённый штрих без предварительного участка»:
    // штрих один и тот же после отрыва, после сохранения и перезагрузки
    // заметки, после отмены и повтора и в контуре экспорта - и кончается в
    // последней точке контакта, а не там, куда дотянулся провизорный хвост.
    void finishedStrokeIsTheSameEverywhere()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString notePath = dir.filePath(QStringLiteral("stroke.uqn"));
        NoteStore store;
        QVERIFY(store.create(notePath, fixtures::textbook()));
        store.attach(&m_ink);

        // Быстрая дуга: провизорный хвост длинный до самого отрыва.
        m_canvas.goToPage(0);
        const QPointF center(450, 350);
        qreal time = 11.0;
        QPointF lastView;
        for (int i = 0; i <= 60; ++i) {
            lastView = center + QPointF(120.0 * std::cos(i * 0.06), 120.0 * std::sin(i * 0.06));
            if (i == 0)
                QVERIFY(m_canvas.beginInkAt(lastView, 0.6, false, time));
            else
                m_canvas.continueInkAt(lastView, 0.6, time);
            time += 1.0 / 240.0;
        }
        m_canvas.endInk();

        QCOMPARE(m_ink.strokeCount(), 1);
        const Stroke drawn = m_ink.allStrokes().first().stroke;
        QCOMPARE(drawn.style, StrokeStyle::Tapered);
        const QPointF lastContact = m_canvas.viewToPage(drawn.page, lastView);
        const QPointF drawnEnd = drawn.points.constLast().position;
        QVERIFY2(std::hypot(drawnEnd.x() - lastContact.x(), drawnEnd.y() - lastContact.y()) < 1e-6,
                 "штрих кончается не в точке отрыва");

        // Файл хранит координаты во float, поэтому сравнение с допуском.
        const auto samePoints = [](const QList<InkPoint> &a, const QList<InkPoint> &b,
                                   qreal tolerance) {
            if (a.size() != b.size())
                return false;
            for (int i = 0; i < a.size(); ++i) {
                if (std::hypot(a[i].position.x() - b[i].position.x(),
                               a[i].position.y() - b[i].position.y())
                        > tolerance
                    || std::abs(a[i].pressure - b[i].pressure) > 1e-4) {
                    return false;
                }
            }
            return true;
        };

        // Сохранение и перезагрузка.
        QVERIFY(store.flush());
        {
            NoteStore reopened;
            InkDocument restored;
            QVERIFY(reopened.openNote(notePath));
            QVERIFY(reopened.load(&restored));
            QCOMPARE(restored.strokeCount(), 1);
            QVERIFY2(samePoints(restored.allStrokes().first().stroke.points, drawn.points, 1e-3),
                     "штрих из файла не совпал с нанесённым");
            QCOMPARE(restored.allStrokes().first().stroke.style, drawn.style);
        }

        // Отмена и повтор.
        m_ink.undo();
        QCOMPARE(m_ink.strokeCount(), 0);
        m_ink.redo();
        QCOMPARE(m_ink.strokeCount(), 1);
        QVERIFY2(samePoints(m_ink.allStrokes().first().stroke.points, drawn.points, 0.0),
                 "штрих после отмены и повтора не совпал с нанесённым");
        QCOMPARE(m_ink.allStrokes().first().stroke.style, drawn.style);

        // Экспорт вписывает контур из тех же точек; центр дуги его конечного
        // торца - точка отрыва.
        StrokeTessellator tessellator;
        tessellator.setWidth(drawn.width);
        tessellator.setStyle(drawn.style);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(drawn.points);
        QVERIFY(!tessellator.outline(drawn.points).isEmpty());
        // У всех вершин конечного торца центр - точка отрыва (design.md, D7).
        const QPointF capCenter = strip.constLast().center();
        QVERIFY2(std::hypot(capCenter.x() - lastContact.x(), capCenter.y() - lastContact.y())
                     < 1e-3,
                 "торец контура экспорта не в точке отрыва");
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

    // --- Вкладыши (задачи 4.5, 5.1-5.5 add-ink-inserts) --------------------

    // Задача 4.5: геометрия чернил вкладыша живёт по зоне удержания и только у
    // раскрытого вкладыша.
    void insertGeometryFollowsOpenStateAndRetention()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        QVERIFY(id != 0);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 0);

        drawLine(&m_canvas, QPointF(420, 180), QPointF(520, 200));
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 1);

        m_canvas.closeInsert(id);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 0);
        m_canvas.openInsert(id);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 1);

        m_canvas.goToPage(13);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 0);
        m_canvas.goToPage(0);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 1);
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
    }

    // Сценарий «Вкладыш в месте касания».
    void insertToolPlacesOpenInsertAndRestoresTool()
    {
        m_canvas.goToPage(0);
        const QPointF at(400, 150);
        const int page = m_canvas.pageAtView(at);
        QCOMPARE(page, 0);

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        m_canvas.setTool(DocumentCanvas::Tool::Insert);
        m_canvas.beginInkAt(at, 0.6, false);
        // Инструмент уже вернулся к ластику, но касание до отрыва ничего не
        // стирает и не рисует.
        m_canvas.continueInkAt(at + QPointF(30, 20), 0.6);
        m_canvas.endInk();

        QCOMPARE(m_ink.insertCount(), 1);
        const InkInsert insert = m_ink.allInserts().first();
        QVERIFY(insert.open);
        QCOMPARE(insert.page, page);
        const QPointF expected = m_canvas.viewToPage(page, at);
        QVERIFY(std::hypot(insert.rect.left() - expected.x(), insert.rect.top() - expected.y())
                < 1e-6);
        QCOMPARE(insert.rect.size(), DocumentCanvas::defaultInsertSize());
        QCOMPARE(m_canvas.tool(), DocumentCanvas::Tool::Eraser);
        QCOMPARE(m_ink.strokeCount(), 0);
    }

    // Сценарий «Касание мимо страницы».
    void insertToolIgnoresGapBetweenPages()
    {
        m_canvas.setZoom(0.4);
        const QRectF first = m_canvas.layout().pageRect(0);
        const QPointF gap = m_canvas.documentToView(
            QPointF(first.center().x(), first.bottom() + m_canvas.layout().rowSpacing() / 2));
        QVERIFY(QRectF(0, 0, m_canvas.width(), m_canvas.height()).contains(gap));
        QCOMPARE(m_canvas.pageAtView(gap), -1);

        m_canvas.setTool(DocumentCanvas::Tool::Insert);
        m_canvas.beginInkAt(gap, 0.6, false);
        m_canvas.endInk();

        QCOMPARE(m_ink.insertCount(), 0);
        QCOMPARE(m_canvas.tool(), DocumentCanvas::Tool::Insert);
    }

    // Сценарий «Касание внутри раскрытого вкладыша».
    void insertToolIgnoresOpenInsert()
    {
        m_canvas.goToPage(0);
        QVERIFY(placeInsert(QPointF(400, 150)) != 0);

        m_canvas.setTool(DocumentCanvas::Tool::Insert);
        m_canvas.beginInkAt(QPointF(450, 200), 0.6, false);
        m_canvas.endInk();

        QCOMPARE(m_ink.insertCount(), 1);
        QCOMPARE(m_canvas.tool(), DocumentCanvas::Tool::Insert);
    }

    // Сценарий «Касание пальцем».
    void fingerNeverPlacesInsert()
    {
        m_canvas.goToPage(0);
        m_canvas.setTool(DocumentCanvas::Tool::Insert);

        const QEventPoint pressed(1, QEventPoint::State::Pressed, QPointF(450, 350),
                                  QPointF(450, 350));
        QTouchEvent begin(QEvent::TouchBegin, touchscreen(), Qt::NoModifier, {pressed});
        QCoreApplication::sendEvent(&m_canvas, &begin);
        const QEventPoint released(1, QEventPoint::State::Released, QPointF(450, 350),
                                   QPointF(450, 350));
        QTouchEvent end(QEvent::TouchEnd, touchscreen(), Qt::NoModifier, {released});
        QCoreApplication::sendEvent(&m_canvas, &end);

        QCOMPARE(m_ink.insertCount(), 0);
        QCOMPARE(m_canvas.tool(), DocumentCanvas::Tool::Insert);
    }

    // Сценарий «Штрих внутри вкладыша».
    void strokeInsideInsertBelongsToInsert()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        m_canvas.setPenWidth(1.6);
        m_canvas.setPenColor(QColor(0xc0, 0x28, 0x28));

        const QPointF from(420, 180);
        drawLine(&m_canvas, from, QPointF(520, 200));

        QVERIFY(m_ink.allStrokes().isEmpty());
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
        const Stroke &stroke = m_ink.strokesInInsert(id).first().stroke;
        QCOMPARE(stroke.insert, id);
        QCOMPARE(stroke.page, 0);
        QVERIFY(qAbs(stroke.width - 1.6) < 1e-9);
        QCOMPARE(stroke.color, QColor(0xc0, 0x28, 0x28));

        // Точки - в локальных координатах вкладыша.
        const QPointF expected = m_canvas.viewToPage(0, from) - m_ink.insertById(id).rect.topLeft();
        const QPointF stored = stroke.points.first().position;
        QVERIFY2(std::hypot(stored.x() - expected.x(), stored.y() - expected.y()) < 1.0,
                 "штрих вкладыша записан не в его локальных координатах");
    }

    // Сценарий «Штрих выходит за край вкладыша».
    void strokeLeavingInsertStaysInInsert()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 180), QPointF(650, 600));

        QVERIFY(m_ink.allStrokes().isEmpty());
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
        const InkInsert insert = m_ink.insertById(id);
        const QPointF last = m_ink.strokesInInsert(id).first().stroke.points.last().position;
        QVERIFY(last.x() > insert.rect.width() || last.y() > insert.rect.height());
    }

    // Сценарий «Штрих страницы под вкладышем».
    void pageStrokeThroughInsertStaysOnPage()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(300, 200), QPointF(650, 220));

        QCOMPARE(m_ink.strokesOnPage(0).size(), 1);
        QVERIFY(m_ink.strokesInInsert(id).isEmpty());
    }

    // Сценарий «Ластик во вкладыше».
    void insertEraserKeepsPageInkBelow()
    {
        m_canvas.goToPage(0);
        drawLine(&m_canvas, QPointF(420, 200), QPointF(520, 200)); // уйдёт под вкладыш
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 240), QPointF(520, 240));
        QCOMPARE(m_ink.strokesOnPage(0).size(), 1);
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        drawLine(&m_canvas, QPointF(470, 195), QPointF(470, 250));

        QVERIFY(m_ink.strokesInInsert(id).isEmpty());
        QCOMPARE(m_ink.strokesOnPage(0).size(), 1);
    }

    // Сценарий «Ластик страницы заходит под вкладыш».
    void pageEraserSkipsInsertAndCoveredInk()
    {
        m_canvas.goToPage(0);
        drawLine(&m_canvas, QPointF(420, 200), QPointF(520, 200)); // закрыт вкладышем
        const quint64 covered = m_ink.strokesOnPage(0).first().id;
        drawLine(&m_canvas, QPointF(440, 120), QPointF(500, 120)); // виден, над рамкой
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 240), QPointF(520, 240));

        m_canvas.setTool(DocumentCanvas::Tool::Eraser);
        drawLine(&m_canvas, QPointF(470, 100), QPointF(470, 300));

        QCOMPARE(m_ink.strokesOnPage(0).size(), 1);
        QCOMPARE(m_ink.strokesOnPage(0).first().id, covered);
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
    }

    // Сценарий «Перевёрнутое перо во вкладыше».
    void invertedPenErasesInsideInsert()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 220), QPointF(520, 220));

        // Перевёрнутый конец касается прямо штриха: через прямой вызов признак
        // ластика действует в точке касания, как в
        // invertedPenErasesAndRestoresPreviousTool.
        drawLine(&m_canvas, QPointF(470, 215), QPointF(470, 225), 0.6, true);
        QVERIFY(m_ink.strokesInInsert(id).isEmpty());

        drawLine(&m_canvas, QPointF(420, 200), QPointF(520, 210));
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
        QVERIFY(m_ink.allStrokes().isEmpty());
    }

    // Сценарий «Увеличение вкладыша».
    void resizeMovesOnlyBottomRightCorner()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 180), QPointF(520, 200));
        const InkInsert before = m_ink.insertById(id);
        const QList<InkPoint> points = m_ink.strokesInInsert(id).first().stroke.points;

        const QRectF doc = m_canvas.insertDocumentRect(id);
        m_canvas.resizeInsert(id, m_canvas.documentToView(doc.bottomRight() + QPointF(100, 60)));

        const InkInsert after = m_ink.insertById(id);
        QCOMPARE(after.rect.topLeft(), before.rect.topLeft());
        QVERIFY(qAbs(after.rect.width() - (before.rect.width() + 100)) < 1e-6);
        QVERIFY(qAbs(after.rect.height() - (before.rect.height() + 60)) < 1e-6);
        const QList<InkPoint> moved = m_ink.strokesInInsert(id).first().stroke.points;
        QCOMPARE(moved.size(), points.size());
        for (int i = 0; i < points.size(); ++i)
            QCOMPARE(moved[i].position, points[i].position);
    }

    // Сценарии «Скрытые чернила возвращаются» и «Предел уменьшения».
    void shrinkingHidesButKeepsInk()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(410, 160), QPointF(550, 250));
        const QRectF doc = m_canvas.insertDocumentRect(id);

        m_canvas.resizeInsert(id, m_canvas.documentToView(doc.topLeft() + QPointF(60, 40)));
        QCOMPARE(m_ink.insertById(id).rect.size(), QSizeF(60, 40));
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);

        m_canvas.resizeInsert(id, m_canvas.documentToView(doc.bottomRight()));
        QVERIFY(qAbs(m_ink.insertById(id).rect.width() - doc.width()) < 1e-6);
        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 1);

        m_canvas.resizeInsert(id, m_canvas.documentToView(doc.topLeft() - QPointF(10, 10)));
        QCOMPARE(m_ink.insertById(id).rect.size(), DocumentCanvas::minInsertSize());
        QCOMPARE(m_ink.insertById(id).rect.topLeft(), doc.topLeft()
                                                          - m_canvas.layout().pageRect(0).topLeft());
    }

    // Сценарии «Свернуть вкладыш» и «Раскрыть вкладыш».
    void collapseAndReopenKeepContent()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 180), QPointF(520, 200));
        const QRectF rect = m_ink.insertById(id).rect;

        m_canvas.closeInsert(id);
        QVERIFY(m_ink.hasInsert(id));
        QVERIFY(!m_ink.insertById(id).open);
        // Свёрнутый вкладыш штрих не перехватывает: он идёт на страницу.
        QCOMPARE(m_canvas.insertAtView(QPointF(450, 200)), quint64(0));

        m_canvas.openInsert(id);
        QVERIFY(m_ink.insertById(id).open);
        QCOMPARE(m_ink.insertById(id).rect, rect);
        drawLine(&m_canvas, QPointF(420, 220), QPointF(520, 230));
        QCOMPARE(m_ink.strokesInInsert(id).size(), 2);
        QVERIFY(m_ink.allStrokes().isEmpty());
    }

    // Сценарий «Сворачивание пустого вкладыша».
    void collapsingEmptyInsertRemovesIt()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        m_canvas.closeInsert(id);
        QVERIFY(!m_ink.hasInsert(id));

        // Отменяется как удаление: возвращается раскрытым.
        m_ink.undo();
        QVERIFY(m_ink.hasInsert(id));
        QVERIFY(m_ink.insertById(id).open);
        QCOMPARE(m_canvas.insertAtView(QPointF(450, 200)), id);
    }

    // Сценарий «Удаление и отмена».
    void deletingInsertUndoesWithContent()
    {
        m_canvas.goToPage(0);
        const quint64 id = placeInsert(QPointF(400, 150));
        drawLine(&m_canvas, QPointF(420, 180), QPointF(520, 200));
        drawLine(&m_canvas, QPointF(420, 220), QPointF(520, 230));
        const QRectF rect = m_ink.insertById(id).rect;

        m_canvas.deleteInsert(id);
        QVERIFY(!m_ink.hasInsert(id));
        QCOMPARE(m_ink.strokeCount(), 0);
        QCOMPARE(m_canvas.insertModel()->rowCount(), 0);

        m_ink.undo();
        QVERIFY(m_ink.hasInsert(id));
        QCOMPARE(m_ink.insertById(id).rect, rect);
        QCOMPARE(m_ink.strokesInInsert(id).size(), 2);
        QCOMPARE(m_canvas.insertGeometryStrokeCount(id), 2);
    }

    // Сценарий «Масштаб вместе со страницей».
    void insertScalesWithPage()
    {
        m_canvas.setZoom(1.0);
        const QPointF at(450, 300);
        QVERIFY(m_canvas.pageAtView(at) >= 0);
        const quint64 id = placeInsert(at);
        const QRectF doc = m_canvas.insertDocumentRect(id);
        const QRectF before(m_canvas.documentToView(doc.topLeft()),
                            m_canvas.documentToView(doc.bottomRight()));

        m_canvas.zoomAt(before.center(), 2.0);
        const QRectF after(m_canvas.documentToView(doc.topLeft()),
                           m_canvas.documentToView(doc.bottomRight()));
        QCOMPARE(m_canvas.insertDocumentRect(id), doc);
        QVERIFY(qAbs(after.width() - 2.0 * before.width()) < 1e-6);
        QVERIFY(qAbs(after.height() - 2.0 * before.height()) < 1e-6);
        // Точка документа под центром зума осталась там же.
        QVERIFY(std::hypot(after.center().x() - before.center().x(),
                           after.center().y() - before.center().y())
                < 1e-6);
        QCOMPARE(m_canvas.insertAtView(before.center()), id);
    }

    // Сценарий «Переключение на разворот».
    void insertSurvivesPageModeSwitch()
    {
        m_canvas.goToPage(3);
        const QPointF at(450, 350);
        const int page = m_canvas.pageAtView(at);
        QVERIFY(page >= 0);
        const quint64 id = placeInsert(at);
        const QRectF rect = m_ink.insertById(id).rect;

        for (const auto mode : {DocumentCanvas::PageMode::Spread, DocumentCanvas::PageMode::Single}) {
            m_canvas.setPageMode(mode);
            QCOMPARE(m_ink.insertById(id).rect, rect);
            const QRectF doc = m_canvas.insertDocumentRect(id);
            QCOMPARE(doc.topLeft() - m_canvas.layout().pageRect(page).topLeft(), rect.topLeft());

            // Модель для QML отдаёт ту же рамку в координатах документа.
            QAbstractItemModel *model = m_canvas.insertModel();
            bool found = false;
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row, 0);
                if (model->data(index, model->roleNames().key("insertId")).toULongLong() != id)
                    continue;
                QCOMPARE(model->data(index, model->roleNames().key("docRect")).toRectF(), doc);
                found = true;
            }
            QVERIFY(found);
        }
    }

    // Сценарий «Вкладыш у края страницы».
    void insertOverhangingIntoRightPageTakesStrokes()
    {
        m_canvas.setPageMode(DocumentCanvas::PageMode::Spread);
        m_canvas.setZoom(0.5);
        const QList<int> pair = m_canvas.layout().pagesInRow(1);
        QCOMPARE(pair.size(), 2);
        m_canvas.goToPage(pair.first());
        const QRectF left = m_canvas.layout().pageRect(pair.first());
        const QRectF right = m_canvas.layout().pageRect(pair.last());

        const quint64 id =
            placeInsert(m_canvas.documentToView(QPointF(left.right() - 30, left.top() + 100)));
        QVERIFY(id != 0);
        QVERIFY(m_canvas.insertDocumentRect(id).right() > right.left() + 100);

        const QPointF from = m_canvas.documentToView(QPointF(right.left() + 20, left.top() + 130));
        const QPointF to = m_canvas.documentToView(QPointF(right.left() + 100, left.top() + 150));
        QCOMPARE(m_canvas.pageAtView(from), pair.last());
        QCOMPARE(m_canvas.insertAtView(from), id);
        drawLine(&m_canvas, from, to);

        QCOMPARE(m_ink.strokesInInsert(id).size(), 1);
        QVERIFY(m_ink.strokesOnPage(pair.last()).isEmpty());
        m_canvas.setPageMode(DocumentCanvas::PageMode::Single);
    }

    // Сценарий «Перекрывающиеся вкладыши».
    void lastOpenedInsertIsOnTop()
    {
        m_canvas.goToPage(0);
        const QPointF first(380, 150);
        const quint64 a = placeInsert(first);
        drawLine(&m_canvas, QPointF(390, 160), QPointF(420, 170)); // чтобы не удалился
        m_canvas.closeInsert(a);
        const quint64 b = placeInsert(first + QPointF(60, 50));
        QVERIFY(a != 0 && b != 0 && a != b);

        const QPointF overlap = first + QPointF(80, 70);
        QCOMPARE(m_canvas.insertAtView(overlap), b);

        m_canvas.openInsert(a);
        QCOMPARE(m_canvas.insertAtView(overlap), a);
        drawLine(&m_canvas, overlap, overlap + QPointF(20, 10));
        QCOMPARE(m_ink.strokesInInsert(a).size(), 2);
        QVERIFY(m_ink.strokesInInsert(b).isEmpty());

        m_canvas.openInsert(b);
        QCOMPARE(m_canvas.insertAtView(overlap), b);
        QCOMPARE(m_canvas.insertZOrder(), QList<quint64>({a, b}));
    }

private:
    // Ставит вкладыш касанием инструментом «Вкладыш» и возвращает его id.
    quint64 placeInsert(const QPointF &viewPoint)
    {
        const int before = m_ink.insertCount();
        m_canvas.setTool(DocumentCanvas::Tool::Insert);
        m_canvas.beginInkAt(viewPoint, 0.6, false);
        m_canvas.endInk();
        if (m_ink.insertCount() != before + 1)
            return 0;
        return m_canvas.insertZOrder().constLast();
    }

    PdfDocumentModel m_document;
    InkDocument m_ink;
    DocumentCanvas m_canvas;
};

QTEST_MAIN(TestCanvasInk)
#include "tst_canvasink.moc"
