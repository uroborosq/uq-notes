// Задачи 5.6, 5.7, 5.8: чернила в координатах страницы, ластик, отмена и
// повтор. Сценарии из specs/ink-input/spec.md.

#include <QRegularExpression>
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

    // ink-beauty-pass, сценарий «Отмена и повтор» требования «Закон толщины
    // хранится со штрихом»: отмена, повтор и отмена стирания возвращают штрих
    // с тем же законом толщины.
    void undoRedoKeepStrokeStyle()
    {
        InkDocument ink;
        Stroke stroke = lineOnPage(1, QPointF(5, 5), QPointF(80, 40));
        stroke.style = StrokeStyle::Tapered;
        const quint64 tapered = ink.addStroke(stroke);
        const quint64 legacy = ink.addStroke(lineOnPage(1, QPointF(5, 60), QPointF(80, 90)));
        const auto styleOf = [&ink](quint64 id) {
            for (const InkDocument::Record &record : ink.strokesOnPage(1)) {
                if (record.id == id)
                    return record.stroke.style;
            }
            return StrokeStyle(-1);
        };

        ink.undo();
        ink.undo();
        ink.redo();
        ink.redo();
        QCOMPARE(styleOf(tapered), StrokeStyle::Tapered);
        QCOMPARE(styleOf(legacy), StrokeStyle::Legacy);

        ink.eraseStrokes({tapered, legacy});
        QCOMPARE(ink.strokeCount(), 0);
        ink.undo();
        QCOMPARE(styleOf(tapered), StrokeStyle::Tapered);
        QCOMPARE(styleOf(legacy), StrokeStyle::Legacy);
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

    // --- Вкладыши (задачи 1.2-1.5 add-ink-inserts) ------------------------

    // Задача 1.2: чернила вкладыша не видны как чернила страницы.
    void insertInkIsSeparateFromPageInk()
    {
        InkDocument ink;
        const quint64 pageStroke = ink.addStroke(lineOnPage(2, QPointF(0, 0), QPointF(50, 50)));
        const quint64 insert = ink.addInsert(2, QRectF(100, 100, 200, 140));
        QVERIFY(insert != 0);
        QVERIFY(ink.insertById(insert).open);

        QSignalSpy pageChanged(&ink, &InkDocument::pageChanged);
        QSignalSpy insertInk(&ink, &InkDocument::insertInkChanged);
        const quint64 insertStroke = ink.addStroke(inInsert(insert, QPointF(5, 5), QPointF(60, 20)));
        QVERIFY(insertStroke != 0);
        // Геометрия страницы от штриха вкладыша не пересобирается.
        QCOMPARE(pageChanged.size(), 0);
        QCOMPARE(insertInk.size(), 1);

        QCOMPARE(ink.strokesOnPage(2).size(), 1);
        QCOMPARE(ink.strokesOnPage(2).first().id, pageStroke);
        QCOMPARE(ink.allStrokes().size(), 1);
        QCOMPARE(ink.strokesInInsert(insert).size(), 1);
        QCOMPARE(ink.strokesInInsert(insert).first().id, insertStroke);
        // Страница у штриха вкладыша - страница вкладыша, точки - локальные.
        QCOMPARE(ink.strokesInInsert(insert).first().stroke.page, 2);
        QCOMPARE(ink.strokesInInsert(insert).first().stroke.points.first().position,
                 QPointF(5, 5));
        QCOMPARE(ink.strokeCount(), 2);

        // Ластик страницы не видит вкладыш, ластик вкладыша - страницу.
        QVERIFY(ink.strokesNear(2, QPointF(30, 12), 4.0).isEmpty());
        QCOMPARE(ink.strokesNearInInsert(insert, QPointF(30, 12), 4.0),
                 QList<quint64>({insertStroke}));
        QVERIFY(ink.strokesNearInInsert(insert, QPointF(25, 25), 1.0).isEmpty());
    }

    void insertStrokeRequiresExistingInsert()
    {
        InkDocument ink;
        QCOMPARE(ink.addStroke(inInsert(42, QPointF(0, 0), QPointF(10, 10))), quint64(0));
        QCOMPARE(ink.addInsert(0, QRectF(0, 0, 0, 10)), quint64(0));
        QCOMPARE(ink.addInsert(-1, QRectF(0, 0, 10, 10)), quint64(0));
        QCOMPARE(ink.insertCount(), 0);
    }

    void insertAtFindsTopOpenInsert()
    {
        InkDocument ink;
        const quint64 lower = ink.addInsert(0, QRectF(0, 0, 100, 100));
        const quint64 upper = ink.addInsert(0, QRectF(50, 50, 100, 100));
        QCOMPARE(ink.insertAt(0, QPointF(10, 10)), lower);
        QCOMPARE(ink.insertAt(0, QPointF(75, 75)), upper);
        QCOMPARE(ink.insertAt(1, QPointF(75, 75)), quint64(0));

        ink.setInsertOpen(upper, false);
        QCOMPARE(ink.insertAt(0, QPointF(75, 75)), lower);
        QCOMPARE(ink.insertsOnPage(0).size(), 2);
    }

    // Задача 1.3: удаление вкладыша с тремя штрихами - одно действие.
    void removingInsertUndoesAsOne()
    {
        InkDocument ink;
        const QRectF rect(40, 60, 180, 120);
        const quint64 insert = ink.addInsert(5, rect);
        QList<quint64> ids;
        for (int i = 0; i < 3; ++i)
            ids.append(ink.addStroke(inInsert(insert, QPointF(0, i * 10), QPointF(90, i * 10))));
        ink.setInsertRect(insert, QRectF(40, 60, 240, 90));

        QSignalSpy removed(&ink, &InkDocument::insertRemoved);
        ink.removeInsert(insert);
        QCOMPARE(removed.size(), 1);
        QVERIFY(!ink.hasInsert(insert));
        QCOMPARE(ink.strokeCount(), 0);
        QVERIFY(ink.strokesInInsert(insert).isEmpty());

        ink.undo();
        QVERIFY(ink.hasInsert(insert));
        QCOMPARE(ink.insertById(insert).rect, QRectF(40, 60, 240, 90));
        QCOMPARE(ink.insertById(insert).page, 5);
        QCOMPARE(ink.strokesInInsert(insert).size(), 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(ink.strokesInInsert(insert)[i].id, ids[i]);

        ink.redo();
        QVERIFY(!ink.hasInsert(insert));
        QCOMPARE(ink.strokeCount(), 0);
    }

    // Сценарий «Отмена не трогает размер».
    void undoAfterResizeUndoesStrokeNotSize()
    {
        InkDocument ink;
        const quint64 insert = ink.addInsert(0, QRectF(0, 0, 100, 100));
        ink.addStroke(inInsert(insert, QPointF(0, 0), QPointF(50, 50)));
        ink.setInsertRect(insert, QRectF(0, 0, 300, 200));

        ink.undo();
        QCOMPARE(ink.strokesInInsert(insert).size(), 0);
        QCOMPARE(ink.insertById(insert).rect, QRectF(0, 0, 300, 200));
    }

    // Сценарий «Отмена штриха во вкладыше»: общая история.
    void undoOrderSpansPageAndInsert()
    {
        InkDocument ink;
        const quint64 insert = ink.addInsert(0, QRectF(0, 0, 100, 100));
        ink.addStroke(lineOnPage(0, QPointF(200, 200), QPointF(300, 300)));
        ink.addStroke(inInsert(insert, QPointF(0, 0), QPointF(50, 50)));

        ink.undo();
        QCOMPARE(ink.strokesInInsert(insert).size(), 0);
        QCOMPARE(ink.strokesOnPage(0).size(), 1);
        ink.undo();
        QCOMPARE(ink.strokesOnPage(0).size(), 0);
    }

    // Создание, размер и раскрытие не обрывают ветку повтора.
    void insertEditsKeepRedoBranch()
    {
        InkDocument ink;
        ink.addStroke(lineOnPage(0, QPointF(0, 0), QPointF(10, 10)));
        ink.undo();
        QVERIFY(ink.canRedo());

        const quint64 insert = ink.addInsert(0, QRectF(0, 0, 100, 100));
        ink.setInsertRect(insert, QRectF(0, 0, 120, 100));
        ink.setInsertOpen(insert, false);
        QVERIFY(ink.canRedo());
        QVERIFY(!ink.canUndo());
    }

    // Задача 1.4: ни одна запись истории не применяется к несуществующему
    // вкладышу.
    void historyNeverReferencesMissingInsert()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral("несуществующий вкладыш")));

        InkDocument ink;
        const quint64 a = ink.addInsert(0, QRectF(0, 0, 100, 100));
        const quint64 b = ink.addInsert(1, QRectF(0, 0, 100, 100));
        const quint64 s1 = ink.addStroke(inInsert(a, QPointF(0, 10), QPointF(90, 10)));
        ink.addStroke(inInsert(a, QPointF(0, 30), QPointF(90, 30)));
        ink.addStroke(inInsert(b, QPointF(0, 30), QPointF(90, 30)));
        ink.addStroke(lineOnPage(0, QPointF(0, 0), QPointF(10, 10)));
        ink.eraseStrokes({s1});
        ink.removeInsert(a);
        ink.addStroke(inInsert(b, QPointF(0, 50), QPointF(90, 50)));
        ink.removeInsert(b);

        int undone = 0;
        while (ink.canUndo()) {
            ink.undo();
            ++undone;
        }
        QCOMPARE(undone, 8);
        QCOMPARE(ink.strokeCount(), 0);
        QCOMPARE(ink.insertCount(), 2); // создание в историю не идёт

        while (ink.canRedo())
            ink.redo();
        QCOMPARE(ink.insertCount(), 0);
        QCOMPARE(ink.strokeCount(), 1); // остался штрих страницы
    }

    // Задача 1.5: загрузка вкладышей мимо истории.
    void loadRestoresInsertsWithoutHistory()
    {
        InkDocument ink;
        InkInsert open{3, 1, QRectF(10, 20, 200, 140), true};
        InkInsert closed{7, 4, QRectF(0, 0, 60, 40), false};

        QList<InkDocument::Record> records;
        records.append({10, lineOnPage(1, QPointF(0, 0), QPointF(10, 10))});
        records.append({11, inInsert(3, QPointF(0, 0), QPointF(10, 10))});
        records.append({12, inInsert(7, QPointF(0, 0), QPointF(10, 10))});
        // Штрих пропавшего вкладыша не должен оказаться на странице.
        records.append({13, inInsert(99, QPointF(0, 0), QPointF(10, 10))});

        QSignalSpy reloaded(&ink, &InkDocument::reloaded);
        ink.loadStrokes(records, 14, {open, closed}, 8);
        QCOMPARE(reloaded.size(), 1);
        QVERIFY(!ink.canUndo());
        QCOMPARE(ink.insertCount(), 2);
        QVERIFY(ink.insertById(3).open);
        QVERIFY(!ink.insertById(7).open);
        QCOMPARE(ink.insertById(3).rect, QRectF(10, 20, 200, 140));
        QCOMPARE(ink.strokesInInsert(3).size(), 1);
        QCOMPARE(ink.strokesInInsert(7).first().stroke.page, 4);
        QCOMPARE(ink.allStrokes().size(), 1);
        QCOMPARE(ink.strokeCount(), 3);
        QCOMPARE(ink.addInsert(0, QRectF(0, 0, 10, 10)), quint64(8));

        ink.clear();
        QCOMPARE(ink.insertCount(), 0);
        QCOMPARE(ink.strokeCount(), 0);
    }

private:
    static Stroke inInsert(quint64 insert, const QPointF &from, const QPointF &to)
    {
        Stroke stroke = lineOnPage(0, from, to);
        stroke.insert = insert;
        return stroke;
    }
};

QTEST_APPLESS_MAIN(TestInkDocument)
#include "tst_inkdocument.moc"
