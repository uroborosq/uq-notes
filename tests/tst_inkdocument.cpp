// Задачи 5.6, 5.7, 5.8: чернила в координатах страницы, ластик, отмена и
// повтор. Сценарии из specs/ink-input/spec.md.

#include <QSignalSpy>
#include <QTest>

#include "ink/inkdocument.h"

class TestInkDocument : public QObject
{
    Q_OBJECT

private:
    static Stroke lineOnPage(int page, const QPointF &from, const QPointF &to, qreal width = 2.0)
    {
        Stroke stroke;
        stroke.page = page;
        stroke.width = width;
        for (int i = 0; i <= 20; ++i) {
            const qreal t = qreal(i) / 20.0;
            stroke.points.append({from + (to - from) * t, 0.5, i / 200.0});
        }
        return stroke;
    }

private slots:
    void storesStrokesPerPageInPageCoordinates()
    {
        InkDocument ink;
        const quint64 a = ink.addStroke(lineOnPage(3, QPointF(10, 10), QPointF(100, 60)));
        const quint64 b = ink.addStroke(lineOnPage(7, QPointF(0, 0), QPointF(50, 50)));

        QVERIFY(a != 0);
        QVERIFY(b != 0);
        QCOMPARE(ink.strokeCount(), 2);
        QCOMPARE(ink.pagesWithInk(), QList<int>({3, 7}));
        QCOMPARE(ink.strokesOnPage(3).size(), 1);
        QCOMPARE(ink.strokesOnPage(7).size(), 1);
        QVERIFY(ink.strokesOnPage(4).isEmpty());

        // Координаты сохранены как есть - это координаты страницы, никакого
        // пересчёта в них не происходит ни при каком масштабе.
        QCOMPARE(ink.strokesOnPage(3).first().stroke.points.first().position, QPointF(10, 10));
        QCOMPARE(ink.strokesOnPage(3).first().stroke.points.last().position, QPointF(100, 60));
    }

    void rejectsDegenerateStrokes()
    {
        InkDocument ink;
        Stroke empty;
        QCOMPARE(ink.addStroke(empty), quint64(0));

        Stroke noPage = lineOnPage(-1, QPointF(0, 0), QPointF(1, 1));
        QCOMPARE(ink.addStroke(noPage), quint64(0));

        Stroke single;
        single.page = 0;
        single.points.append({QPointF(1, 1), 0.5, 0.0});
        QCOMPARE(ink.addStroke(single), quint64(0));
        QCOMPARE(ink.strokeCount(), 0);
    }

    // Сценарий «Стирание по касанию»: ластик находит задетые штрихи.
    void eraserFindsStrokesItTouches()
    {
        InkDocument ink;
        const quint64 crossed = ink.addStroke(lineOnPage(0, QPointF(0, 50), QPointF(100, 50)));
        ink.addStroke(lineOnPage(0, QPointF(0, 200), QPointF(100, 200)));

        // Прямо по линии.
        QCOMPARE(ink.strokesNear(0, QPointF(50, 50), 4.0), QList<quint64>({crossed}));
        // Рядом, в пределах радиуса.
        QCOMPARE(ink.strokesNear(0, QPointF(50, 54), 4.0), QList<quint64>({crossed}));
        // Далеко - ничего.
        QVERIFY(ink.strokesNear(0, QPointF(50, 120), 4.0).isEmpty());
        // Соседняя страница не задета.
        QVERIFY(ink.strokesNear(1, QPointF(50, 50), 4.0).isEmpty());
    }

    void eraseRemovesOnlyTouchedStrokes()
    {
        InkDocument ink;
        const quint64 first = ink.addStroke(lineOnPage(0, QPointF(0, 50), QPointF(100, 50)));
        const quint64 second = ink.addStroke(lineOnPage(0, QPointF(0, 200), QPointF(100, 200)));

        ink.eraseStrokes({first});
        QCOMPARE(ink.strokeCount(), 1);
        QCOMPARE(ink.strokesOnPage(0).first().id, second);
    }

    // Сценарий «Отмена штриха»: исчезает ровно последний.
    void undoRemovesExactlyTheLastStroke()
    {
        InkDocument ink;
        const quint64 first = ink.addStroke(lineOnPage(0, QPointF(0, 0), QPointF(10, 10)));
        ink.addStroke(lineOnPage(0, QPointF(20, 20), QPointF(30, 30)));
        QCOMPARE(ink.strokeCount(), 2);

        QVERIFY(ink.canUndo());
        ink.undo();
        QCOMPARE(ink.strokeCount(), 1);
        QCOMPARE(ink.strokesOnPage(0).first().id, first);
    }

    // Сценарий «Повтор после отмены»: штрих возвращается с той же геометрией
    // и толщиной.
    void redoRestoresStrokeIdentically()
    {
        InkDocument ink;
        const Stroke original = lineOnPage(2, QPointF(5, 5), QPointF(80, 40), 3.7);
        const quint64 id = ink.addStroke(original);

        ink.undo();
        QCOMPARE(ink.strokeCount(), 0);
        QVERIFY(ink.canRedo());

        ink.redo();
        QCOMPARE(ink.strokeCount(), 1);

        const InkDocument::Record &restored = ink.strokesOnPage(2).first();
        QCOMPARE(restored.id, id);
        QCOMPARE(restored.stroke.width, original.width);
        QCOMPARE(restored.stroke.points.size(), original.points.size());
        for (int i = 0; i < original.points.size(); ++i) {
            QCOMPARE(restored.stroke.points[i].position, original.points[i].position);
            QCOMPARE(restored.stroke.points[i].pressure, original.points[i].pressure);
        }
    }

    // Гранулярность: одно движение ластика - одна отмена, сколько бы штрихов
    // оно ни стёрло.
    void oneEraserActionUndoesAsOne()
    {
        InkDocument ink;
        QList<quint64> ids;
        for (int i = 0; i < 4; ++i)
            ids.append(ink.addStroke(lineOnPage(0, QPointF(0, i * 10), QPointF(100, i * 10))));

        ink.eraseStrokes(ids);
        QCOMPARE(ink.strokeCount(), 0);

        ink.undo();
        QCOMPARE(ink.strokeCount(), 4); // вернулись все четыре разом

        ink.redo();
        QCOMPARE(ink.strokeCount(), 0);
    }

    void undoRedoWalksTheWholeHistory()
    {
        InkDocument ink;
        for (int i = 0; i < 5; ++i)
            ink.addStroke(lineOnPage(0, QPointF(0, i), QPointF(10, i)));

        for (int i = 5; i > 0; --i) {
            QCOMPARE(ink.strokeCount(), i);
            ink.undo();
        }
        QCOMPARE(ink.strokeCount(), 0);
        QVERIFY(!ink.canUndo());
        ink.undo(); // лишняя отмена ничего не портит
        QCOMPARE(ink.strokeCount(), 0);

        for (int i = 1; i <= 5; ++i) {
            ink.redo();
            QCOMPARE(ink.strokeCount(), i);
        }
        QVERIFY(!ink.canRedo());
        ink.redo();
        QCOMPARE(ink.strokeCount(), 5);
    }

    void newStrokeDropsRedoBranch()
    {
        InkDocument ink;
        ink.addStroke(lineOnPage(0, QPointF(0, 0), QPointF(10, 10)));
        ink.undo();
        QVERIFY(ink.canRedo());

        ink.addStroke(lineOnPage(0, QPointF(50, 50), QPointF(60, 60)));
        QVERIFY(!ink.canRedo());
        QCOMPARE(ink.strokeCount(), 1);
    }

    void emitsChangesForIncrementalSaving()
    {
        InkDocument ink;
        QSignalSpy added(&ink, &InkDocument::strokeAdded);
        QSignalSpy removed(&ink, &InkDocument::strokeRemoved);

        const quint64 id = ink.addStroke(lineOnPage(4, QPointF(0, 0), QPointF(10, 10)));
        QCOMPARE(added.size(), 1);
        QCOMPARE(added.first().at(0).toULongLong(), id);
        QCOMPARE(added.first().at(1).toInt(), 4);

        ink.eraseStrokes({id});
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().at(0).toULongLong(), id);

        // Отмена стирания - это снова добавление, и запись в файл об этом
        // узнаёт тем же сигналом.
        ink.undo();
        QCOMPARE(added.size(), 2);
    }

    void loadStrokesBypassesHistory()
    {
        InkDocument ink;
        QList<InkDocument::Record> records;
        records.append({10, lineOnPage(1, QPointF(0, 0), QPointF(10, 10))});
        records.append({11, lineOnPage(2, QPointF(0, 0), QPointF(10, 10))});

        ink.loadStrokes(records, 12);
        QCOMPARE(ink.strokeCount(), 2);
        QVERIFY(!ink.canUndo());
        QVERIFY(!ink.canRedo());
        QCOMPARE(ink.nextId(), quint64(12));

        // Новые штрихи получают идентификаторы после загруженных.
        QCOMPARE(ink.addStroke(lineOnPage(3, QPointF(0, 0), QPointF(5, 5))), quint64(12));
    }
};

QTEST_APPLESS_MAIN(TestInkDocument)
#include "tst_inkdocument.moc"
