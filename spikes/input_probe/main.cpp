#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QSocketNotifier>

#include <csignal>
#include <cstring>

#include <unistd.h>

#include "probeitem.h"

namespace {

bool hasArgument(int argc, char *argv[], const char *name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return true;
    }
    return false;
}

int s_signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(s_signalPipe[1], &byte, 1);
}

} // namespace

int main(int argc, char *argv[])
{
    // --no-compress: сжатие мышиных перемещений выключается до создания
    // приложения. На Wayland это не действует: конструктор QWaylandIntegration
    // безусловно включает атрибут обратно (qtbase 6.11,
    // qwaylandintegration.cpp), и сводка показывает ON. Для пера это неважно:
    // оно идёт планшетным путём мимо этого сжатия (spikes/FINDINGS.md, частота
    // отсчётов пера). Флаг имеет смысл на X11.
    if (hasArgument(argc, argv, "--no-compress"))
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    // --rate: подсчёт частоты отсчётов пера без построчного журнала.
    ProbeItem::setRateMode(hasArgument(argc, argv, "--rate"));

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("uq-input-probe"));

    // Ctrl+C в терминале завершает пробник штатно, со сводкой: замер частоты
    // удобнее останавливать из того же терминала, где идёт libinput
    // debug-events. Обработчик сигнала только пишет байт в канал, выход идёт
    // уже из цикла событий.
    if (::pipe(s_signalPipe) == 0) {
        auto *notifier = new QSocketNotifier(s_signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
    }

    QQmlApplicationEngine engine;
    engine.loadFromModule("UqInputProbe", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    const int code = app.exec();

    for (QObject *root : engine.rootObjects()) {
        if (auto *probe = root->findChild<ProbeItem *>())
            probe->dumpSummary();
    }
    return code;
}
