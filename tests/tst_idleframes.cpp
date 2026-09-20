// Задача 2.2 fix-idle-repaint-loop: требование «Отсутствие кадров в простое»
// из specs/idle-power/spec.md, проверяемое напрямую - по кадрам.
//
// Единственный тест в наборе, который идёт НЕ под QT_QPA_PLATFORM=offscreen и
// открывает настоящее окно. Инвариант «тесты не зависят от наличия дисплея»
// здесь нарушен сознательно и ровно один раз: под offscreen цикл рендера
// крутится вхолостую всегда - на чистом дереве там 183 кадра за 2 секунды при
// нулевом расходе процессора, - поэтому требование о покое там непроверяемо ни
// по одному сигналу окна (design.md, D2).
//
// Перед замером идёт канарейка: тест заставляет окно перерисоваться и
// убеждается, что кадры вообще наблюдаемы. Без неё тест проходил бы вхолостую
// именно там, где бесполезен: при погашенном экране или невыведенном окне
// композитор не шлёт frame callbacks, кадров нет ни у исправного дерева, ни у
// сломанного, и «ноль кадров» ничего не значит.

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "canvas/documentcanvas.h"
#include "fixtures.h"

namespace {

// Считает кадры, выданные окном. frameSwapped приходит с потока рендера,
// поэтому подписка идёт через объект-контекст на потоке теста: соединение по
// умолчанию ставит вызов в очередь, и счётчик не делится между потоками.
class FrameCounter : public QObject
{
public:
    explicit FrameCounter(QQuickWindow *window)
    {
        connect(window, &QQuickWindow::frameSwapped, this, [this] { ++m_count; });
    }

    int count() const { return m_count; }
    void reset() { m_count = 0; }

private:
    int m_count = 0;
};

constexpr int kQuietWindowMs = 400;
constexpr int kSettleTimeoutMs = 8000;
// Окно замера заметно длиннее окна устаканивания: «покой» не должен оказаться
// случайным просветом между двумя пачками кадров.
constexpr int kIdleWindowMs = 1500;

} // namespace

class TestIdleFrames : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    DocumentCanvas *m_canvas = nullptr;

private slots:
    void init()
    {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            QSKIP("под offscreen кадры выдаются вхолостую всегда: покой там неотличим от петли");

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
        QTRY_VERIFY(m_canvas->width() > 0 && m_canvas->pageCount() > 0);
    }

    void cleanup()
    {
        m_canvas = nullptr;
        m_window = nullptr;
        m_engine.reset();
        m_dir.reset();
    }

    void windowGoesQuiet()
    {
        FrameCounter frames(m_window);

        // Канарейка: сдвиг холста обязан дать кадры. Если их нет, окно не
        // выводится на экран, и «ноль кадров» в простое ничего не докажет.
        frames.reset();
        for (int i = 0; i < 10; ++i) {
            m_canvas->panBy(QPointF(0, i % 2 == 0 ? -40 : 40));
            QTest::qWait(30);
        }
        if (frames.count() == 0)
            QSKIP("окно не выводится на экран: кадры не наблюдаемы, замер покоя недоказателен");

        // Покой должен наступить сам. Ждём промежутка длиной kQuietWindowMs,
        // в котором не выдано ни одного кадра; петля такого промежутка не даёт
        // никогда, поэтому ожидание ограничено сверху.
        QElapsedTimer clock;
        clock.start();
        int lastWindow = -1;
        bool quiet = false;
        while (clock.elapsed() < kSettleTimeoutMs) {
            frames.reset();
            QTest::qWait(kQuietWindowMs);
            lastWindow = frames.count();
            if (lastWindow == 0) {
                quiet = true;
                break;
            }
        }
        QVERIFY2(quiet,
                 qPrintable(QStringLiteral("окно не пришло в покой за %1 мс: последние %2 мс "
                                           "дали %3 кадров")
                                .arg(kSettleTimeoutMs)
                                .arg(kQuietWindowMs)
                                .arg(lastWindow)));

        // И держит его: у исправного покоя промежуточных значений нет.
        frames.reset();
        QTest::qWait(kIdleWindowMs);
        QCOMPARE(frames.count(), 0);
    }
};

QTEST_MAIN(TestIdleFrames)
#include "tst_idleframes.moc"
