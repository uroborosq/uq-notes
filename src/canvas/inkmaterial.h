#pragma once

#include <QColor>
#include <QSGGeometry>
#include <QSGMaterial>

// Материал чернил с аналитическим сглаживанием кромки (design.md
// ink-beauty-pass, D7). Геометрия - полоса AxisVertex; кромку выносит и
// сглаживает шейдер, поэтому MSAA чернилам не нужен, а зум меняет только
// униформы.
//
// Сухие штрихи страницы одного цвета сливаются в батч, как раньше. Мокрый слой
// - отдельный тип: иначе мокрый узел сливается с сухими, и каждое событие пера
// перезаливает VBO всей страницы. Куски мокрого штриха различаются ключом
// батча: каждый кусок - свой батч, и на событие перезаливается только
// изменившийся кусок, а не весь штрих.
class InkMaterial : public QSGMaterial
{
public:
    enum class Layer {
        Dry,
        Wet,
    };

    // Смещения полей униформ-блока buf в шейдерах (std140). tst_inkmaterial
    // сверяет их с отражением собранных .qsb.
    struct Uniforms
    {
        static constexpr int kMatrix = 0;
        static constexpr int kColor = 64;
        static constexpr int kPxPerUnit = 80;
        static constexpr int kMinHalfWidthPx = 84;
        static constexpr int kFringePx = 88;
        static constexpr int kUsedSize = 92;
    };

    explicit InkMaterial(Layer layer = Layer::Dry);

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    Layer layer() const { return m_layer; }

    QColor color() const { return m_color; }
    void setColor(const QColor &color);

    // Пол полуширины, device px (design.md, D7, «Пол толщины»).
    float minHalfWidthPx() const { return m_minHalfWidthPx; }
    void setMinHalfWidthPx(float px);

    int batchKey() const { return m_batchKey; }
    void setBatchKey(int key);

    // Раскладка AxisVertex для QSGGeometry: центр (позиция для батч-рендера),
    // направление, полуширина, s - входы 0..3 шейдера.
    static const QSGGeometry::AttributeSet &attributes();

private:
    Layer m_layer;
    QColor m_color = QColor(Qt::black);
    float m_minHalfWidthPx;
    int m_batchKey = 0;
};
