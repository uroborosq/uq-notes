#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "probeitem.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("uq-input-probe"));

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
