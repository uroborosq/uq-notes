#include "inknodepool.h"

#include <QSGGeometryNode>

#include <cstring>

static_assert(sizeof(AxisVertex) == 24, "раскладка InkMaterial::attributes()");

InkNodePool::InkNodePool(InkMaterial::Layer layer, int firstBatchKey, int size)
    : m_layer(layer)
    , m_firstBatchKey(firstBatchKey)
    , m_node(new QSGNode)
{
    grow(size);
}

int InkNodePool::vertexCount(int index) const
{
    return m_slots.at(index)->geometry()->vertexCount();
}

void InkNodePool::grow(int size)
{
    while (m_slots.size() < size) {
        QSGGeometryNode *slot =
            makeNode(QColor(Qt::black), m_layer, m_firstBatchKey + int(m_slots.size()));
        m_node->appendChildNode(slot);
        m_slots.append(slot);
    }
}

void InkNodePool::fill(int index, const AxisVertex *vertices, int count, const QColor &color)
{
    QSGGeometryNode *node = m_slots.at(index);
    auto *material = static_cast<InkMaterial *>(node->material());
    if (node->geometry()->vertexCount() == 0 || material->color() != color) {
        material->setColor(color);
        node->markDirty(QSGNode::DirtyMaterial);
    }
    upload(node, vertices, count);
}

void InkNodePool::clear(int index)
{
    QSGGeometryNode *node = m_slots.at(index);
    if (node->geometry()->vertexCount() == 0)
        return;
    node->geometry()->allocate(0);
    node->markDirty(QSGNode::DirtyGeometry);
}

QSGGeometryNode *InkNodePool::makeNode(const QColor &color, InkMaterial::Layer layer, int batchKey)
{
    auto *node = new QSGGeometryNode;

    auto *geometry = new QSGGeometry(InkMaterial::attributes(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);

    auto *material = new InkMaterial(layer);
    material->setColor(color);
    material->setBatchKey(batchKey);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);

    return node;
}

void InkNodePool::upload(QSGGeometryNode *node, const AxisVertex *vertices, int count)
{
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(count);
    std::memcpy(geometry->vertexData(), vertices, size_t(count) * sizeof(AxisVertex));
    node->markDirty(QSGNode::DirtyGeometry);
}
