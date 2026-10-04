// Задачи 3.1-3.6 add-pen-only-mode: сценарии specs/pen-only-mode/spec.md.
//
// Тест поднимает настоящее окно приложения (offscreen): режим решает судьбу
// касания по месту в окне, и проверить его можно только на собранном окне.
// Касания идут через QTest::touchEvent, то есть через платформу и
// QGuiApplication с её синтезом мышиных событий из касаний (design.md, D8), а
// не прямой отправкой элементу. Перо - мышиное событие с устройством Stylus,
// как оно приходит на целевом устройстве (spikes/FINDINGS.md, 1.1).

#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "input/touchguard.h"

namespace {

// Qt держит устройства ввода в глобальном списке, а события ссылаются на них по
// указателю, поэтому устройства создаются один раз и живут до конца процесса -
// как в tst_canvasink.
QPointingDevice *touchscreen()
{
    static QPointingDevice *device =
        QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
    return device;
}

const QPointingDevice *stylus()
{
    static auto *device = new QPointingDevice(
        QStringLiteral("test stylus"), 2, QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    return device;
}

const QPointingDevice *touchpad()
{
    static auto *device = new QPointingDevice(
        QStringLiteral("test touchpad"), 4, QInputDevice::DeviceType::TouchPad,
        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 2, 0);
    return device;
}

void sendMouse(QQuickWindow *window, QEvent::Type type, const QPointF &scenePoint,
               Qt::MouseButton button, Qt::MouseButtons buttons, const QPointingDevice *device)
{
    QMouseEvent event(type, scenePoint, scenePoint, window->mapToGlobal(scenePoint), button,
                      buttons, Qt::NoModifier, device);
    QCoreApplication::sendEvent(window, &event);
}

void click(QQuickWindow *window, const QPointF &scenePoint, const QPointingDevice *device)
{
    sendMouse(window, QEvent::MouseButtonPress, scenePoint, Qt::LeftButton, Qt::LeftButton, device);
    sendMouse(window, QEvent::MouseButtonRelease, scenePoint, Qt::LeftButton, Qt::NoButton, device);
    QCoreApplication::processEvents();
}

void penClick(QQuickWindow *window, const QPointF &scenePoint)
{
    click(window, scenePoint, stylus());
}

void mouseClick(QQuickWindow *window, const QPointF &scenePoint)
{
    click(window, scenePoint, QPointingDevice::primaryPointingDevice());
}

void penDrag(QQuickWindow *window, const QPointF &from, const QPointF &to, int steps)
{
    sendMouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, stylus());
    for (int i = 1; i <= steps; ++i) {
        sendMouse(window, QEvent::MouseMove, from + (to - from) * (qreal(i) / steps),
                  Qt::NoButton, Qt::LeftButton, stylus());
    }
    sendMouse(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, stylus());
    QCoreApplication::processEvents();
}

// Qt Quick придерживает TouchUpdate до следующего кадра и отдаёт обработчикам
// только последний. Без кадра между шагами движение пальца дошло бы до жестов
// разом на отрыве, и документ сдвигала бы одна инерция.
void waitForFrame()
{
    QTest::qWait(20);
}

// Касание одним пальцем, которое тест ведёт по шагам: между шагами может
// случиться что угодно - нажатие пером, смена режима.
class Finger
{
public:
    Finger(QQuickWindow *window, const QPointF &at)
        : m_window(window)
        , m_sequence(QTest::touchEvent(window, touchscreen(), false))
        , m_at(at)
    {
        m_sequence.press(0, m_at.toPoint(), m_window).commit();
    }

    void moveBy(const QPointF &step, int count)
    {
        for (int i = 0; i < count; ++i) {
            m_at += step;
            m_sequence.move(0, m_at.toPoint(), m_window).commit();
            waitForFrame();
        }
    }

    void release()
    {
        m_sequence.release(0, m_at.toPoint(), m_window).commit();
    }

private:
    QQuickWindow *m_window;
    QTest::QTouchEventSequence m_sequence;
    QPointF m_at;
};

void fingerTap(QQuickWindow *window, const QPointF &at)
{
    Finger finger(window, at);
    finger.release();
}

void fingerDrag(QQuickWindow *window, const QPointF &from, const QPointF &step, int count)
{
    Finger finger(window, from);
    finger.moveBy(step, count);
    finger.release();
}

// Сведение-разведение двух пальцев симметрично вокруг центра.
void twoFingerPinch(QQuickWindow *window, const QPointF &center, qreal spreadPerStep, int count)
{
    QTest::QTouchEventSequence sequence = QTest::touchEvent(window, touchscreen(), false);
    QPointF a = center - QPointF(40, 0);
    QPointF b = center + QPointF(40, 0);
    sequence.press(0, a.toPoint(), window).press(1, b.toPoint(), window).commit();
    for (int i = 0; i < count; ++i) {
        a -= QPointF(spreadPerStep, 0);
        b += QPointF(spreadPerStep, 0);
        sequence.move(0, a.toPoint(), window).move(1, b.toPoint(), window).commit();
        waitForFrame();
    }
    sequence.release(0, a.toPoint(), window).release(1, b.toPoint(), window).commit();
}

// Ладонь лежит на странице, а второй палец касается другого места и
// отрывается: обе точки - одна последовательность касаний.
void tapWhilePalmRests(QQuickWindow *window, const QPointF &palm, const QPointF &tap)
{
    QTest::QTouchEventSequence sequence = QTest::touchEvent(window, touchscreen(), false);
    sequence.press(0, palm.toPoint(), window).commit();
    waitForFrame();
    sequence.stationary(0).press(1, tap.toPoint(), window).commit();
    waitForFrame();
    sequence.stationary(0).release(1, tap.toPoint(), window).commit();
    waitForFrame();
    sequence.release(0, palm.toPoint(), window).commit();
}

// Делегаты Repeater и TreeView не всегда получают QObject-родителя, поэтому
// поиск идёт по дереву элементов, а не через findChild.
QQuickItem *findItem(QQuickItem *root, const std::function<bool(QQuickItem *)> &match)
{
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem *findVisibleNamed(QQuickItem *root, const QString &objectName)
{
    return findItem(root, [&objectName](QQuickItem *item) {
        return item->objectName() == objectName && item->isVisible();
    });
}

QQuickItem *findVisibleText(QQuickItem *root, const QString &text)
{
    return findItem(root, [&text](QQuickItem *item) {
        return item->isVisible() && item->property("text").toString() == text;
    });
}

QPointF centerOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
}

QPointF originOf(const DocumentCanvas *canvas)
{
    return QPointF(canvas->originX(), canvas->originY());
}

} // namespace

class TestPenOnlyMode : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    DocumentCanvas *m_canvas = nullptr;
    TouchGuard *m_guard = nullptr;

    QPointF canvasCenter() const
    {
        return m_canvas->mapToScene(QPointF(m_canvas->width() / 2.0, m_canvas->height() / 2.0));
    }

    QQuickItem *button(const QString &objectName) const
    {
        return findVisibleNamed(m_window->contentItem(), objectName);
    }

    // Инерция, запущенная предыдущим шагом, не должна выдавать себя за
    // сдвиг от следующего.
    void settle()
    {
        QCoreApplication::processEvents();
        m_canvas->stopFlick();
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        const QString pdf = m_dir->filePath(QStringLiteral("book.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));

        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("initialFile"), pdf);
        m_engine->loadFromModule("UqNotes", "Main");
        QVERIFY(!m_engine->rootObjects().isEmpty());
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        m_canvas = m_window->findChild<DocumentCanvas *>(QStringLiteral("canvas"));
        QVERIFY(m_canvas);
        m_guard = m_window->findChild<TouchGuard *>(QStringLiteral("touchGuard"));
        QVERIFY(m_guard);
        QTRY_VERIFY(m_canvas->width() > 0 && m_canvas->pageCount() > 0);
        m_canvas->goToPage(0);
        settle();
    }

    void cleanup()
    {
        m_guard = nullptr;
        m_canvas = nullptr;
        m_window = nullptr;
        m_engine.reset();
        m_dir.reset();
    }

    // Сценарии «Перезапуск», «Включение пером», «Выключение пером» и «Попытка
    // выключить пальцем».
    void toggleWithPenMouseAndFinger()
    {
        QVERIFY(!m_guard->penOnly());
        QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
        QVERIFY(toggle);
        QVERIFY(!toggle->property("checked").toBool());

        penClick(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());
        QVERIFY(toggle->property("checked").toBool());

        mouseClick(m_window, centerOf(toggle));
        QVERIFY(!m_guard->penOnly());
        QVERIFY(!toggle->property("checked").toBool());

        fingerTap(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());

        fingerTap(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());
        QVERIFY(toggle->property("checked").toBool());
    }

    // Сценарии «Сдвиг одним пальцем» и «Pinch двумя пальцами».
    void fingerNavigationWorksInMode()
    {
        m_guard->setPenOnly(true);

        const QPointF before = originOf(m_canvas);
        Finger finger(m_window, canvasCenter());
        finger.moveBy(QPointF(0, -15), 8);
        // Сдвиг идёт вслед за пальцем, а не только инерцией на отрыве.
        QVERIFY2(originOf(m_canvas) != before, "палец в режиме не сдвинул документ");
        finger.release();
        QVERIFY(m_canvas->isFlicking());
        settle();

        const qreal zoom = m_canvas->zoom();
        twoFingerPinch(m_window, canvasCenter(), 12, 8);
        settle();
        QVERIFY2(m_canvas->zoom() > zoom * 1.2,
                 qPrintable(QStringLiteral("масштаб %1 после %2").arg(m_canvas->zoom()).arg(zoom)));
        QVERIFY(m_guard->penOnly());
    }

    // Сценарии «Касание элементов вкладыша», «Касание панели инструментов»,
    // «Касание пункта оглавления» и «Элементы окна пером».
    void fingerTapsOnControlsAreIgnoredPenTapsWork()
    {
        InkDocument *ink = m_canvas->ink();
        QVERIFY(ink);
        const quint64 insert = ink->addInsert(0, QRectF(120, 160, 200, 140));
        ink->setInsertOpen(insert, false);
        QCoreApplication::processEvents();

        QQuickItem *anchor = button(QStringLiteral("insertAnchor"));
        QVERIFY(anchor);
        QQuickItem *eraser = button(QStringLiteral("eraserButton"));
        QVERIFY(eraser);
        QQuickItem *conclusion = nullptr;
        QTRY_VERIFY((conclusion = findVisibleText(m_window->contentItem(),
                                                  QStringLiteral("4 Conclusion"))));
        QCOMPARE(m_canvas->currentPage(), 0);

        m_guard->setPenOnly(true);

        fingerTap(m_window, centerOf(anchor));
        QVERIFY(!ink->insertById(insert).open);
        fingerTap(m_window, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);
        fingerTap(m_window, centerOf(conclusion));
        QTest::qWait(50);
        QCOMPARE(m_canvas->currentPage(), 0);
        QCOMPARE(ink->strokeCount(), 0);

        penClick(m_window, centerOf(anchor));
        QVERIFY(ink->insertById(insert).open);
        penClick(m_window, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Eraser);
        penClick(m_window, centerOf(conclusion));
        QTRY_COMPARE(m_canvas->currentPage(), 12);
        QCOMPARE(ink->strokeCount(), 0);
    }

    // ink-beauty-pass, D5, сценарий «Касание элемента окна пером»: касание
    // пером кнопки, значка вкладыша или пункта оглавления срабатывает на
    // элементе, а точки на странице не оставляет - без режима и в режиме
    // «Только перо». Контроль: то же касание по странице точку оставляет,
    // значит, точек не было потому, что касание забрал элемент.
    void penTapsOnControlsLeaveNoDot()
    {
        InkDocument *ink = m_canvas->ink();
        QVERIFY(ink);
        const quint64 insert = ink->addInsert(0, QRectF(120, 160, 200, 140));
        ink->setInsertOpen(insert, false);
        QCoreApplication::processEvents();

        QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
        QQuickItem *anchor = button(QStringLiteral("insertAnchor"));
        QVERIFY(toggle);
        QVERIFY(anchor);
        QQuickItem *conclusion = nullptr;
        QTRY_VERIFY((conclusion = findVisibleText(m_window->contentItem(),
                                                  QStringLiteral("4 Conclusion"))));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);

        // Без режима: кнопка панели.
        penClick(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());
        QCOMPARE(ink->strokeCount(), 0);

        // В режиме: значок вкладыша и пункт оглавления.
        penClick(m_window, centerOf(anchor));
        QVERIFY(ink->insertById(insert).open);
        QCOMPARE(ink->strokeCount(), 0);
        penClick(m_window, centerOf(conclusion));
        QTRY_COMPARE(m_canvas->currentPage(), 12);
        QCOMPARE(ink->strokeCount(), 0);

        // Касание страницы, которым закрывают всплывающую панель пера, панель
        // закрывает и точки не оставляет - ни пером, ни мышью. У мыши нет
        // нажима, и правило задева (D5) такое касание не отбросило бы: оно не
        // должно доходить до холста вовсе.
        settle();
        QVERIFY(m_canvas->pageAtView(m_canvas->mapFromScene(canvasCenter())) >= 0);
        auto *penPopup = m_window->findChild<QObject *>(QStringLiteral("penPopup"));
        QVERIFY(penPopup);
        for (const QPointingDevice *device : {stylus(), QPointingDevice::primaryPointingDevice()}) {
            QMetaObject::invokeMethod(penPopup, "open");
            QTRY_VERIFY(penPopup->property("opened").toBool());
            click(m_window, canvasCenter(), device);
            QTRY_VERIFY(!penPopup->property("visible").toBool());
            QCOMPARE(ink->strokeCount(), 0);
        }

        // Контроль: касание страницы тем же пером - точка.
        penClick(m_window, canvasCenter());
        QCOMPARE(ink->strokeCount(), 1);
    }

    // Сценарий «Касание кнопки при ладони на странице»: вторая точка той же
    // последовательности касаний отклоняется, а ладонь остаётся навигацией.
    void fingerTapWhilePalmRestsOnPage()
    {
        QQuickItem *eraser = button(QStringLiteral("eraserButton"));
        QVERIFY(eraser);
        const QPointF palm = canvasCenter() + QPointF(-100, 150);

        // Без режима такое касание кнопку нажимает - иначе проверка ниже
        // ничего бы не доказывала.
        tapWhilePalmRests(m_window, palm, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Eraser);
        m_canvas->setTool(DocumentCanvas::Tool::Pen);

        m_guard->setPenOnly(true);
        tapWhilePalmRests(m_window, palm, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);

        // Ладонь, к которой в пересобранном событии приходит палец на кнопке,
        // по-прежнему двигает документ.
        QTest::QTouchEventSequence sequence = QTest::touchEvent(m_window, touchscreen(), false);
        QPointF at = palm;
        sequence.press(0, at.toPoint(), m_window).commit();
        waitForFrame();
        sequence.stationary(0).press(1, centerOf(eraser).toPoint(), m_window).commit();
        waitForFrame();
        const QPointF before = originOf(m_canvas);
        for (int i = 0; i < 8; ++i) {
            at += QPointF(0, -15);
            sequence.move(0, at.toPoint(), m_window).stationary(1).commit();
            waitForFrame();
        }
        QVERIFY2(originOf(m_canvas) != before, "ладонь при пальце на кнопке не сдвинула документ");
        sequence.release(0, at.toPoint(), m_window).release(1, centerOf(eraser).toPoint(), m_window)
            .commit();
        settle();
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);
    }

    // Сценарии «Письмо при ладони на экране», «Пинч на тачпаде» и «Мышь».
    void penMouseAndTouchpadStillWork()
    {
        InkDocument *ink = m_canvas->ink();
        QVERIFY(ink);
        m_guard->setPenOnly(true);
        const QPointF center = canvasCenter();

        // Ладонь лежит на экране всё время штриха.
        Finger palm(m_window, center + QPointF(-120, 120));
        penDrag(m_window, center + QPointF(-60, -20), center + QPointF(60, 10), 16);
        QCOMPARE(ink->strokeCount(), 1);
        palm.release();
        QCOMPARE(ink->strokeCount(), 1);
        settle();

        // Пинч на тачпаде - событие жеста окну в точке курсора.
        const qreal zoom = m_canvas->zoom();
        const QPointF global = m_window->mapToGlobal(center);
        for (const auto &[type, value] : {std::pair{Qt::BeginNativeGesture, 0.0},
                                          std::pair{Qt::ZoomNativeGesture, 0.25},
                                          std::pair{Qt::EndNativeGesture, 0.0}}) {
            QNativeGestureEvent event(type, touchpad(), 2, center, center, global, value,
                                      QPointF());
            QCoreApplication::sendEvent(m_window, &event);
        }
        QVERIFY2(m_canvas->zoom() > zoom * 1.2,
                 qPrintable(QStringLiteral("масштаб %1 после %2").arg(m_canvas->zoom()).arg(zoom)));

        // Мышь нажимает кнопки панели.
        QQuickItem *eraser = button(QStringLiteral("eraserButton"));
        QVERIFY(eraser);
        mouseClick(m_window, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Eraser);
    }

    // Сценарий «Включение во время сдвига пальцем».
    void enablingKeepsFingerPanGoing()
    {
        QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
        QVERIFY(toggle);

        Finger finger(m_window, canvasCenter());
        finger.moveBy(QPointF(0, -15), 8);
        penClick(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());

        const QPointF before = originOf(m_canvas);
        finger.moveBy(QPointF(0, -15), 6);
        QVERIFY2(originOf(m_canvas) != before, "сдвиг пальцем остановился на включении режима");
        finger.release();
        settle();
    }

    // Сценарий «Включение при пальце на кнопке».
    void fingerOnButtonBeforeEnablingStillClicks()
    {
        QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
        QQuickItem *eraser = button(QStringLiteral("eraserButton"));
        QVERIFY(toggle);
        QVERIFY(eraser);

        Finger finger(m_window, centerOf(eraser));
        penClick(m_window, centerOf(toggle));
        QVERIFY(m_guard->penOnly());
        finger.release();
        QCoreApplication::processEvents();
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Eraser);
    }

    // Сценарий «Выключение при пальце на кнопке».
    void fingerOnButtonInModeStaysIgnoredAfterDisabling()
    {
        QQuickItem *toggle = button(QStringLiteral("penOnlyButton"));
        QQuickItem *eraser = button(QStringLiteral("eraserButton"));
        QVERIFY(toggle);
        QVERIFY(eraser);
        m_guard->setPenOnly(true);

        Finger finger(m_window, centerOf(eraser));
        penClick(m_window, centerOf(toggle));
        QVERIFY(!m_guard->penOnly());
        finger.release();
        QCoreApplication::processEvents();
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Pen);

        fingerTap(m_window, centerOf(eraser));
        QCOMPARE(m_canvas->tool(), DocumentCanvas::Tool::Eraser);
    }
};

QTEST_MAIN(TestPenOnlyMode)
#include "tst_penonlymode.moc"
