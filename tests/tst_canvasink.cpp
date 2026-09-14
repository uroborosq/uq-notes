// Задачи 5.1, 5.5, 5.6, 5.7, 5.9: разделение указателей по типу устройства,
// сухая геометрия с вытеснением, координаты страницы, ластик и поведение без
// давления. Сценарии из specs/ink-input/spec.md.

#include <QMouseEvent>
#include <QPointingDevice>
#include <QTemporaryDir>
#include <QTest>
#include <QTouchEvent>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "ink/stroketessellator.h"
#include "pdf/pdfdocumentmodel.h"
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
        }

        // Отмена и повтор.
        m_ink.undo();
        QCOMPARE(m_ink.strokeCount(), 0);
        m_ink.redo();
        QCOMPARE(m_ink.strokeCount(), 1);
        QVERIFY2(samePoints(m_ink.allStrokes().first().stroke.points, drawn.points, 0.0),
                 "штрих после отмены и повтора не совпал с нанесённым");

        // Экспорт вписывает контур из тех же точек; его торец - точка отрыва.
        StrokeTessellator tessellator;
        tessellator.setWidth(drawn.width);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(drawn.points);
        QVERIFY(!tessellator.outline(drawn.points).isEmpty());
        const QPointF capCenter((strip[strip.size() - 2].x + strip[strip.size() - 1].x) / 2.0,
                                (strip[strip.size() - 2].y + strip[strip.size() - 1].y) / 2.0);
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

private:
    PdfDocumentModel m_document;
    InkDocument m_ink;
    DocumentCanvas m_canvas;
};

QTEST_MAIN(TestCanvasInk)
#include "tst_canvasink.moc"
