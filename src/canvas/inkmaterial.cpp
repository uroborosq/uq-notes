#include "inkmaterial.h"

#include <QMatrix4x4>
#include <QSGMaterialShader>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

namespace {

QSGMaterialType g_dryInkType;
QSGMaterialType g_wetInkType;

class InkMaterialShader : public QSGMaterialShader
{
public:
    InkMaterialShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/uqnotes/shaders/ink.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/uqnotes/shaders/ink.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial,
                           QSGMaterial *oldMaterial) override
    {
        Q_ASSERT(!oldMaterial || newMaterial->type() == oldMaterial->type());
        const auto *material = static_cast<const InkMaterial *>(newMaterial);
        const auto *previous = static_cast<const InkMaterial *>(oldMaterial);
        QByteArray *buffer = state.uniformData();
        Q_ASSERT(buffer->size() >= InkMaterial::Uniforms::kUsedSize);
        char *data = buffer->data();
        bool changed = false;

        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(data + InkMaterial::Uniforms::kMatrix, matrix.constData(), 64);
            // Device px в точке страницы. Флаг RequiresFullMatrixExceptTranslate
            // оставляет масштаб зума в матрице и в слитом батче: на CPU
            // переносится только сдвиг центра, поэтому hw и n в точках
            // страницы верны.
            const QMatrix4x4 modelView = state.modelViewMatrix();
            const float determinant =
                modelView(0, 0) * modelView(1, 1) - modelView(0, 1) * modelView(1, 0);
            const float pxPerUnit = std::sqrt(std::abs(determinant)) * state.devicePixelRatio();
            std::memcpy(data + InkMaterial::Uniforms::kPxPerUnit, &pxPerUnit, sizeof(float));
            changed = true;
        }

        if (!previous || previous->color() != material->color() || state.isOpacityDirty()) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            float a = 0.0f;
            material->color().getRgbF(&r, &g, &b, &a);
            const float alpha = a * state.opacity();
            const QVector4D premultiplied(r * alpha, g * alpha, b * alpha, alpha);
            std::memcpy(data + InkMaterial::Uniforms::kColor, &premultiplied, 16);
            changed = true;
        }

        if (!previous || previous->minHalfWidthPx() != material->minHalfWidthPx()) {
            const float floor = material->minHalfWidthPx();
            const float fringe = float(ink::kFringePx);
            std::memcpy(data + InkMaterial::Uniforms::kMinHalfWidthPx, &floor, sizeof(float));
            std::memcpy(data + InkMaterial::Uniforms::kFringePx, &fringe, sizeof(float));
            changed = true;
        }

        return changed;
    }
};

template <typename T>
int order(const T &a, const T &b)
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

} // namespace

InkMaterial::InkMaterial(Layer layer)
    : m_layer(layer)
    , m_minHalfWidthPx(float(ink::kDefaultMinHalfWidthPx))
{
    // Бахрома полупрозрачна по построению: смешивание нужно всегда, даже у
    // непрозрачного цвета.
    setFlag(Blending);
    setFlag(RequiresFullMatrixExceptTranslate);
}

QSGMaterialType *InkMaterial::type() const
{
    return m_layer == Layer::Wet ? &g_wetInkType : &g_dryInkType;
}

QSGMaterialShader *InkMaterial::createShader(QSGRendererInterface::RenderMode renderMode) const
{
    Q_UNUSED(renderMode)
    return new InkMaterialShader;
}

int InkMaterial::compare(const QSGMaterial *other) const
{
    // Тип у сравниваемых всегда один: батч-рендер сравнивает только узлы
    // одного type().
    const auto *ink = static_cast<const InkMaterial *>(other);
    if (const int byColor = order(m_color.rgba(), ink->m_color.rgba()))
        return byColor;
    if (const int byFloor = order(m_minHalfWidthPx, ink->m_minHalfWidthPx))
        return byFloor;
    return order(m_batchKey, ink->m_batchKey);
}

void InkMaterial::setColor(const QColor &color)
{
    m_color = color;
}

void InkMaterial::setMinHalfWidthPx(float px)
{
    m_minHalfWidthPx = std::max(px, 0.0f);
}

void InkMaterial::setBatchKey(int key)
{
    m_batchKey = key;
}

const QSGGeometry::AttributeSet &InkMaterial::attributes()
{
    static const QSGGeometry::Attribute data[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true), // center
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),       // dir
        QSGGeometry::Attribute::create(2, 1, QSGGeometry::FloatType),       // hw
        QSGGeometry::Attribute::create(3, 1, QSGGeometry::FloatType),       // s
    };
    static const QSGGeometry::AttributeSet set = {4, int(sizeof(AxisVertex)), data};
    return set;
}
