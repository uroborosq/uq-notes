#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QSurfaceFormat>

#include <cmath>
#include <QImage>
#include <QQuickWindow>
#include <QTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

int main(int argc, char *argv[])
{
    // Кромку чернил сглаживает их материал (InkMaterial, design.md
    // ink-beauty-pass, D7), а не мультисэмплинг. MSAA 4x пока остаётся ради
    // остальной сцены - рамок и значков вкладышей, панелей; выключается он
    // отдельным шагом после аудита этих элементов (D8).
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("uq-notes"));
    app.setOrganizationName(QStringLiteral("uq-notes"));
    app.setApplicationDisplayName(QStringLiteral("uq notes"));
    app.setApplicationVersion(QStringLiteral(UQ_NOTES_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QGuiApplication::translate("main", "Рукописные заметки поверх PDF"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QGuiApplication::translate("main", "PDF или заметка .uqn"));
    parser.process(app);

    QString initialFile;
    if (!parser.positionalArguments().isEmpty())
        initialFile = QFileInfo(parser.positionalArguments().constFirst()).absoluteFilePath();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("initialFile"), initialFile);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("UqNotes", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

#ifdef UQ_DIAGNOSTICS
    // Диагностические хуки: сценарный ввод и снимки экрана. Нужны для замеров
    // и проверки сценариев без человека у экрана, в собранный пакет не входят
    // (сборка с -DUQ_DIAGNOSTICS=OFF).
    // Сценарное письмо: непрерывный поток точек пера в течение заданного
    // числа секунд. Без пера иначе нечем проверить ни то, что геометрия
    // доходит до графа сцены, ни что автосохранение не роняет кадры.
    if (qEnvironmentVariableIsSet("UQ_DEMO_INK")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        const int seconds = qMax(1, qEnvironmentVariableIntValue("UQ_DEMO_INK"));

        QTimer::singleShot(900, window, [window, seconds] {
            QQuickItem *canvas = window->findChild<QQuickItem *>(QStringLiteral("canvas"));
            if (!canvas)
                return;

            auto *clock = new QElapsedTimer;
            clock->start();
            auto *timer = new QTimer(canvas);
            auto *strokeClock = new QElapsedTimer;
            strokeClock->start();

            timer->setInterval(8);
            QObject::connect(timer, &QTimer::timeout, canvas,
                             [canvas, timer, clock, strokeClock, seconds] {
                const qreal w = canvas->width();
                const qreal h = canvas->height();
                const qreal elapsed = clock->elapsed() / 1000.0;

                if (elapsed > seconds) {
                    QMetaObject::invokeMethod(canvas, "endInk");
                    timer->stop();
                    timer->deleteLater();
                    delete clock;
                    delete strokeClock;
                    return;
                }

                // Штрих длиной около двух секунд, потом отрыв и новый.
                bool drawing = false;
                QMetaObject::invokeMethod(canvas, "isDrawing", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, drawing));
                const qreal t = strokeClock->elapsed() / 2000.0;
                const QPointF point(w * (0.25 + 0.5 * t),
                                    h * 0.5 + std::sin(elapsed * 7.0) * h * 0.18);

                if (t >= 1.0) {
                    QMetaObject::invokeMethod(canvas, "endInk");
                    strokeClock->restart();
                    return;
                }
                if (!drawing) {
                    QMetaObject::invokeMethod(canvas, "beginInkAt", Q_ARG(QPointF, point),
                                              Q_ARG(qreal, 0.5), Q_ARG(bool, false));
                    return;
                }
                QMetaObject::invokeMethod(canvas, "continueInkAt", Q_ARG(QPointF, point),
                                          Q_ARG(qreal, 0.15 + 0.8 * std::abs(std::sin(elapsed))));
            });
            timer->start();
        });
    }

    // Сценарная навигация: непрерывный зум и быстрая прокрутка. Ими
    // проверяется бюджет плавности из canvas-view без человека у экрана.
    if (qEnvironmentVariableIsSet("UQ_DEMO_NAV")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        const int seconds = qMax(1, qEnvironmentVariableIntValue("UQ_DEMO_NAV"));

        QTimer::singleShot(900, window, [window, seconds] {
            QQuickItem *canvas = window->findChild<QQuickItem *>(QStringLiteral("canvas"));
            if (!canvas)
                return;

            auto *clock = new QElapsedTimer;
            clock->start();
            auto *timer = new QTimer(canvas);
            timer->setInterval(8);
            QObject::connect(timer, &QTimer::timeout, canvas, [canvas, timer, clock, seconds] {
                const qreal elapsed = clock->elapsed() / 1000.0;
                if (elapsed > seconds) {
                    timer->stop();
                    timer->deleteLater();
                    delete clock;
                    return;
                }

                const QPointF anchor(canvas->width() / 2.0, canvas->height() / 2.0);
                // Чередуем фазы: непрерывный зум туда-обратно и быстрая
                // прокрутка на десятки страниц.
                const int phase = int(elapsed) % 8;
                if (phase < 4) {
                    const qreal factor = phase < 2 ? 1.012 : 1.0 / 1.012;
                    QMetaObject::invokeMethod(canvas, "zoomBy", Q_ARG(QPointF, anchor),
                                              Q_ARG(qreal, factor));
                } else {
                    const qreal direction = phase < 6 ? -1.0 : 1.0;
                    QMetaObject::invokeMethod(canvas, "panBy",
                                              Q_ARG(QPointF, QPointF(0, direction * 260)));
                }
            });
            timer->start();
        });
    }

    // Переход на страницу при старте - тем же способом проверяются подсветка
    // в оглавлении и раскладка разворота.
    if (qEnvironmentVariableIsSet("UQ_GOTO_PAGE")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        const int page = qEnvironmentVariableIntValue("UQ_GOTO_PAGE");
        QTimer::singleShot(800, window, [window, page] {
            if (QQuickItem *canvas = window->findChild<QQuickItem *>(QStringLiteral("canvas")))
                QMetaObject::invokeMethod(canvas, "goToPage", Q_ARG(int, page));
        });
    }

    // Снимок окна для проверки сценариев отображения без человека у экрана
    // (заполнители, разворот, чернила после перезапуска). Включается только
    // переменной окружения и на обычный запуск не влияет.
    if (qEnvironmentVariableIsSet("UQ_SCREENSHOT")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        const QString path = qEnvironmentVariable("UQ_SCREENSHOT");
        const int delayMs = qEnvironmentVariableIntValue("UQ_SCREENSHOT_DELAY_MS");
        QTimer::singleShot(delayMs > 0 ? delayMs : 1500, window, [window, path] {
            if (window)
                window->grabWindow().save(path);
            QCoreApplication::quit();
        });
    }
#endif // UQ_DIAGNOSTICS

    return app.exec();
}
