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
// Второй тест здесь же, потому что ему нужно то же настоящее окно с QML
// приложения: зум и прокрутка не пересобирают сцену целиком (ink-beauty-pass,
// рецензия группы 12). Полная пересборка в батч-рендере перезаливает все
// чернила всех страниц, а её вызывают узлы, добавленные вне корня батча
// (подписи масштаба и страницы в шапке), и удаление или появление корня
// батча (узла страницы). Счёт - по журналу QSG_RENDERER_DEBUG=render.
//
// Перед замером идёт канарейка: тест заставляет окно перерисоваться и
// убеждается, что кадры вообще наблюдаемы. Без неё тест проходил бы вхолостую
// именно там, где бесполезен: при погашенном экране или невыведенном окне
// композитор не шлёт frame callbacks, кадров нет ни у исправного дерева, ни у
// сломанного, и «ноль кадров» ничего не значит.

#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
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

// Журнал рендера (QSG_RENDERER_DEBUG=render, включается в main до создания
// приложения): отладочные сообщения перехватываются и не попадают в вывод
// теста, полные пересборки считаются. Сообщения идут и с потока рендера.
class RebuildLog
{
public:
    RebuildLog()
    {
        s_current = this;
        m_previous = qInstallMessageHandler(&RebuildLog::handle);
    }
    ~RebuildLog()
    {
        qInstallMessageHandler(m_previous);
        s_current = nullptr;
    }

    void reset()
    {
        QMutexLocker lock(&m_mutex);
        m_full = 0;
        m_frames = 0;
    }
    int fullRebuilds()
    {
        QMutexLocker lock(&m_mutex);
        return m_full;
    }
    int frames()
    {
        QMutexLocker lock(&m_mutex);
        return m_frames;
    }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        RebuildLog *log = s_current;
        if (type == QtDebugMsg && log) {
            if (message.startsWith(QStringLiteral("Renderer::render()"))) {
                QMutexLocker lock(&log->m_mutex);
                ++log->m_frames;
                if (message.contains(QStringLiteral("rebuild: full")))
                    ++log->m_full;
            }
            return;
        }
        if (log && log->m_previous)
            log->m_previous(type, context, message);
    }

    static inline RebuildLog *s_current = nullptr;
    QtMessageHandler m_previous = nullptr;
    QMutex m_mutex;
    int m_full = 0;
    int m_frames = 0;
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
    std::unique_ptr<RebuildLog> m_log;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    DocumentCanvas *m_canvas = nullptr;

private slots:
    void init()
    {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            QSKIP("под offscreen кадры выдаются вхолостую всегда: покой там неотличим от петли");

        m_log = std::make_unique<RebuildLog>();
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        const QString pdf = m_dir->filePath(QStringLiteral("book.pdf"));
        // Прокрутке нужен длинный документ: страницы должны входить в зону
        // удержания и уходить из неё.
        const bool long_ = QByteArray(QTest::currentTestFunction()) != "windowGoesQuiet";
        QVERIFY(QFile::copy(long_ ? fixtures::large() : fixtures::textbook(), pdf));

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
        m_log.reset();
    }

    // Зум и быстрая прокрутка: полных пересборок - единицы. Подпись масштаба
    // меняется на каждом кадре зума, подпись страницы - на прокрутке, страницы
    // входят в зону удержания и уходят из неё. Первый проход прогревает запас
    // узлов страниц (новый узел страницы - новый корень батча, это тоже
    // полная пересборка), второй - тот же путь - мерится.
    void zoomAndScrollDoNotRebuildWholeScene()
    {
        FrameCounter frames(m_window);
        const QPointF anchor(m_canvas->width() / 2, m_canvas->height() / 2);
        const auto pass = [&] {
            for (int i = 0; i < 40; ++i) {
                m_canvas->zoomBy(anchor, 1.0 / 1.02);
                QTest::qWait(16);
            }
            for (int i = 0; i < 40; ++i) {
                m_canvas->zoomBy(anchor, 1.02);
                QTest::qWait(16);
            }
            for (int i = 0; i < 60; ++i) {
                m_canvas->panBy(QPointF(0, -200));
                QTest::qWait(16);
            }
            for (int i = 0; i < 60; ++i) {
                m_canvas->panBy(QPointF(0, 200));
                QTest::qWait(16);
            }
            QTest::qWait(300);
        };
        pass();
        if (frames.count() == 0)
            QSKIP("окно не выводится на экран: кадры не наблюдаемы");

        m_log->reset();
        pass();
        const int rendered = m_log->frames();
        const int full = m_log->fullRebuilds();
        qInfo("полных пересборок %d из %d кадров", full, rendered);
        QVERIFY2(rendered > 100,
                 qPrintable(QStringLiteral("кадров рендера за проход: %1").arg(rendered)));
        QVERIFY2(full <= 5, qPrintable(QStringLiteral("полных пересборок %1 из %2 кадров")
                                           .arg(full)
                                           .arg(rendered)));
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

// Журнал рендера батч-рендер читает из окружения один раз, поэтому переменная
// ставится до создания приложения. Сообщения перехватывает RebuildLog.
int main(int argc, char *argv[])
{
    qputenv("QSG_RENDERER_DEBUG", "render");
    QGuiApplication app(argc, argv);
    TestIdleFrames test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_idleframes.moc"
