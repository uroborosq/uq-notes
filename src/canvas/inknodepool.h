#pragma once

#include <QColor>
#include <QList>

#include "canvas/inkmaterial.h"
#include "ink/stroketessellator.h"

class QSGGeometryNode;
class QSGNode;

// Пул узлов-полос InkMaterial под одним узлом-контейнером (design.md
// ink-beauty-pass, D10). Добавление любого узла под корнем батча заставляет
// батч-рендер Qt Quick пересобрать все батчи этого корня, и слитый батч сухих
// чернил плотной страницы (десятки МБ) перезаливается целиком. Поэтому то,
// что появляется и исчезает при письме, - мокрый штрих и недавние штрихи, -
// живёт в слотах, созданных вместе с узлом страницы: слот наполняется и
// пустеет, а узлы не добавляются.
//
// Контейнер и слоты принадлежат графу сцены, как только контейнер включён в
// него: пул их не удаляет и живёт ровно столько же, сколько узел поверхности.
class InkNodePool
{
public:
    // Ключ батча слота i - firstBatchKey + i: слоты не сливаются ни друг с
    // другом, ни со слотами другого пула того же слоя материала.
    InkNodePool(InkMaterial::Layer layer, int firstBatchKey, int size);

    QSGNode *node() const { return m_node; }
    int size() const { return int(m_slots.size()); }
    QSGGeometryNode *slot(int index) const { return m_slots.at(index); }
    int vertexCount(int index) const;

    // Добавляет слоты - пересборка батчей корня; только при переполнении.
    void grow(int size);

    // Кладёт полосу в слот. Пустой слот батча не имеет, а DirtyGeometry у
    // элемента без батча перестройку батчей не заказывает (qsgbatchrenderer,
    // Renderer::nodeChanged), и слот остался бы невидимым. DirtyMaterial
    // заказывает её (BuildBatches): батч получают только элементы без батча,
    // остальные батчи не трогаются.
    void fill(int index, const AxisVertex *vertices, int count, const QColor &color);
    // Пустой слот не попадает ни в один батч и не рисуется.
    void clear(int index);

    // Узел-полоса InkMaterial без вершин.
    static QSGGeometryNode *makeNode(const QColor &color, InkMaterial::Layer layer,
                                     int batchKey = 0);
    // Перезаливает вершины узла.
    static void upload(QSGGeometryNode *node, const AxisVertex *vertices, int count);

private:
    InkMaterial::Layer m_layer;
    int m_firstBatchKey;
    QSGNode *m_node;
    QList<QSGGeometryNode *> m_slots;
};
