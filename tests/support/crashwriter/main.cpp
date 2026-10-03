// Вспомогательный процесс для проверки восстановления после аварийного
// завершения (задача 7.4). Пишет штрихи в заметку с обычным автосохранением и
// ждёт, пока его убьют: сымитировать SIGKILL внутри тестового процесса нельзя.
//
// На каждое сохранение печатает "saved <число штрихов>", чтобы тест знал,
// сколько работы обязано пережить аварию.
//
// Режим "inserts" пишет штрихи не на страницу, а во вкладыш, созданный на
// старте: сценарий «Аварийное завершение во время письма во вкладыше».

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include "ink/inkdocument.h"
#include "storage/notestore.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        QTextStream(stderr)
            << "usage: uq_crash_writer <note.uqn> <source.pdf> [autosaveMs] [inserts]\n";
        return 2;
    }

    const QString notePath = QString::fromLocal8Bit(argv[1]);
    const QString pdfPath = QString::fromLocal8Bit(argv[2]);
    const int autosaveMs = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 5000;
    const bool intoInsert = argc > 4 && QString::fromLocal8Bit(argv[4]) == QLatin1String("inserts");

    auto *ink = new InkDocument(&app);
    auto *store = new NoteStore(&app);
    if (!store->create(notePath, pdfPath)) {
        QTextStream(stderr) << "create failed: " << store->errorString() << "\n";
        return 1;
    }
    store->attach(ink);
    store->setAutosaveIntervalMs(autosaveMs);

    const quint64 insert = intoInsert ? ink->addInsert(2, QRectF(60, 80, 400, 500)) : 0;

    QObject::connect(store, &NoteStore::saved, [ink] {
        QTextStream out(stdout);
        out << "saved " << ink->strokeCount() << "\n";
        out.flush();
    });

    // Непрерывное письмо: штрих каждые несколько миллисекунд.
    auto *writer = new QTimer(&app);
    writer->setInterval(10);
    int counter = 0;
    QObject::connect(writer, &QTimer::timeout, [ink, insert, &counter] {
        Stroke stroke;
        stroke.page = counter % 12;
        stroke.insert = insert;
        stroke.width = 2.0;
        // Каждый второй штрих - заострённый: восстановление после сбоя
        // проверяет и колонку закона толщины (ink-beauty-pass, D3).
        stroke.style = counter % 2 == 1 ? StrokeStyle::Tapered : StrokeStyle::Legacy;
        for (int i = 0; i <= 24; ++i)
            stroke.points.append({QPointF(20 + i * 3.0, 40 + counter % 400), 0.5, i / 200.0});
        ink->addStroke(stroke);
        ++counter;
    });
    writer->start();

    QTextStream(stdout) << "ready\n";
    return app.exec();
}
