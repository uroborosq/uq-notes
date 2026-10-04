// ink-beauty-pass, D7: материал чернил. Тип мокрого слоя отличается от сухого
// (иначе мокрый узел сливается в батч с сухими, и каждое событие пера
// перезаливает VBO страницы), compare() группирует по цвету и полу, флаги
// оставляют масштаб в матрице, шейдеры собраны батчируемыми, а раскладка
// униформ и входов совпадает с тем, что пишет C++. Цвет в униформе -
// премультиплицированный и умноженный на непрозрачность узла: так его
// ждёт смешивание over (ink-input, «Цветные чернила на тёмной странице»).

#include <QFile>
#include <QVector4D>
#include <QHash>
#include <QTest>

#include <private/qsgrenderer_p.h>
#include <rhi/qshader.h>

#include <cstring>
#include <memory>

#include "canvas/inkmaterial.h"
#include "ink/stroketessellator.h"

namespace {

QShader loadShader(const QString &name)
{
    QFile file(QStringLiteral(":/uqnotes/shaders/") + name + QStringLiteral(".qsb"));
    if (!file.open(QIODevice::ReadOnly))
        return QShader();
    return QShader::fromSerialized(file.readAll());
}

// Состояние рендера для updateUniformData() без графа сцены и GPU:
// RenderState читает непрозрачность и буфер униформ у QSGRenderer, которым
// он создан.
class UniformProbe : public QSGRenderer
{
public:
    UniformProbe()
        : QSGRenderer(nullptr)
    {
    }

    QVector4D colorAfterUpdate(const QColor &color, qreal opacity)
    {
        InkMaterial material;
        material.setColor(color);
        std::unique_ptr<QSGMaterialShader> shader(
            material.createShader(QSGRendererInterface::RenderMode2D));
        QByteArray uniforms(InkMaterial::Uniforms::kUsedSize, '\0');
        m_current_opacity = opacity;
        m_current_uniform_data = &uniforms;
        QSGMaterialShader::RenderState renderState =
            state(QSGMaterialShader::RenderState::DirtyOpacity);
        const bool changed = shader->updateUniformData(renderState, &material, nullptr);
        m_current_uniform_data = nullptr;
        if (!changed)
            return QVector4D(-1, -1, -1, -1);
        QVector4D written;
        std::memcpy(&written, uniforms.constData() + InkMaterial::Uniforms::kColor, 16);
        return written;
    }

protected:
    void render() override { }
};

} // namespace

class TestInkMaterial : public QObject
{
    Q_OBJECT

private slots:
    void wetAndDryLayersHaveDistinctTypes()
    {
        InkMaterial dry;
        InkMaterial otherDry;
        InkMaterial wet(InkMaterial::Layer::Wet);
        QCOMPARE(dry.type(), otherDry.type());
        QVERIFY(dry.type() != wet.type());
        QCOMPARE(wet.layer(), InkMaterial::Layer::Wet);
    }

    void compareGroupsByColorFloorAndBatchKey()
    {
        InkMaterial a;
        InkMaterial b;
        a.setColor(QColor(0x1c, 0x2a, 0x5a));
        b.setColor(QColor(0x1c, 0x2a, 0x5a));
        QCOMPARE(a.compare(&b), 0);

        b.setColor(QColor(200, 20, 30));
        QVERIFY(a.compare(&b) != 0);
        QCOMPARE(a.compare(&b), -b.compare(&a));

        b.setColor(a.color());
        b.setMinHalfWidthPx(0.3f);
        QVERIFY(a.compare(&b) != 0);

        b.setMinHalfWidthPx(a.minHalfWidthPx());
        b.setBatchKey(1);
        QVERIFY2(a.compare(&b) != 0, "куски мокрого штриха обязаны не сливаться в один батч");
    }

    void materialBlendsAndKeepsScaleInMatrix()
    {
        InkMaterial material;
        const QSGMaterial::Flags flags = material.flags();
        QVERIFY(flags.testFlag(QSGMaterial::Blending));
        // Масштаб - в матрице, на CPU переносится только сдвиг центра.
        QVERIFY(flags.testFlags(QSGMaterial::RequiresFullMatrixExceptTranslate));
        QVERIFY(!flags.testFlags(QSGMaterial::RequiresFullMatrix));
        QVERIFY(!flags.testFlag(QSGMaterial::NoBatching));
    }

    // Пол толщины - состояние материала (униформ), а не геометрии.
    void floorIsMaterialState()
    {
        InkMaterial material;
        QCOMPARE(material.minHalfWidthPx(), 0.15f);
        material.setMinHalfWidthPx(0.3f);
        QCOMPARE(material.minHalfWidthPx(), 0.3f);
    }

    void attributesDescribeAxisVertex()
    {
        const QSGGeometry::AttributeSet &set = InkMaterial::attributes();
        QCOMPARE(set.count, 4);
        QCOMPARE(set.stride, int(sizeof(AxisVertex)));
        QCOMPARE(set.stride, 24);
        const int tuples[] = {2, 2, 1, 1};
        for (int i = 0; i < set.count; ++i) {
            QCOMPARE(set.attributes[i].position, i);
            QCOMPARE(set.attributes[i].tupleSize, tuples[i]);
            QCOMPARE(set.attributes[i].type, int(QSGGeometry::FloatType));
            // Позиция для батч-рендера - только центр: его он сдвигает на CPU.
            QCOMPARE(bool(set.attributes[i].isVertexCoordinate), i == 0);
        }
    }

    void shadersAreBuiltBatchable()
    {
        const QShader vertex = loadShader(QStringLiteral("ink.vert"));
        const QShader fragment = loadShader(QStringLiteral("ink.frag"));
        QVERIFY2(vertex.isValid(), "нет :/uqnotes/shaders/ink.vert.qsb");
        QVERIFY2(fragment.isValid(), "нет :/uqnotes/shaders/ink.frag.qsb");
        QCOMPARE(vertex.stage(), QShader::VertexStage);
        QCOMPARE(fragment.stage(), QShader::FragmentStage);
        bool batchable = false;
        for (const QShaderKey &key : vertex.availableShaders())
            batchable = batchable || key.sourceVariant() == QShader::BatchableVertexShader;
        QVERIFY2(batchable, "вершинный шейдер собран без BATCHABLE");
    }

    void vertexInputsMatchAttributes()
    {
        const QShader vertex = loadShader(QStringLiteral("ink.vert"));
        QHash<QByteArray, int> locations;
        for (const QShaderDescription::InOutVariable &input : vertex.description().inputVariables())
            locations.insert(input.name, input.location);
        QCOMPARE(locations.value("center", -1), 0);
        QCOMPARE(locations.value("dir", -1), 1);
        QCOMPARE(locations.value("hw", -1), 2);
        QCOMPARE(locations.value("s", -1), 3);
    }

    // Смещения в униформ-блоке - те, по которым пишет updateUniformData().
    void uniformLayoutMatchesMaterial()
    {
        for (const QString &name : {QStringLiteral("ink.vert"), QStringLiteral("ink.frag")}) {
            const QShader shader = loadShader(name);
            const QList<QShaderDescription::UniformBlock> blocks =
                shader.description().uniformBlocks();
            QCOMPARE(blocks.size(), 1);
            QCOMPARE(blocks.first().binding, 0);
            QVERIFY(blocks.first().size >= InkMaterial::Uniforms::kUsedSize);
            QHash<QByteArray, int> offsets;
            for (const QShaderDescription::BlockVariable &member : blocks.first().members)
                offsets.insert(member.name, member.offset);
            QCOMPARE(offsets.value("qt_Matrix", -1), InkMaterial::Uniforms::kMatrix);
            QCOMPARE(offsets.value("color", -1), InkMaterial::Uniforms::kColor);
            QCOMPARE(offsets.value("pxPerUnit", -1), InkMaterial::Uniforms::kPxPerUnit);
            QCOMPARE(offsets.value("minHwPx", -1), InkMaterial::Uniforms::kMinHalfWidthPx);
            QCOMPARE(offsets.value("fringePx", -1), InkMaterial::Uniforms::kFringePx);
        }
    }

    // Униформ цвета - премультиплицированный цвет, умноженный на
    // непрозрачность узла: шейдер умножает его на покрытие, а смешивание
    // over (ONE, ONE_MINUS_SRC_ALPHA) складывает с фоном. Непремультиплицированный
    // цвет у полупрозрачного узла выходил бы ярче, чем надо, а альфа без
    // цвета гасила бы чернила до фона.
    void colorUniformIsPremultipliedByOpacity()
    {
        UniformProbe probe;
        const auto close = [](const QVector4D &a, const QVector4D &b) {
            return (a - b).length() < 1e-6f;
        };
        const QVector4D red = probe.colorAfterUpdate(QColor::fromRgbF(1, 0, 0, 1), 0.5);
        QVERIFY2(close(red, QVector4D(0.5f, 0, 0, 0.5f)),
                 qPrintable(QStringLiteral("красный при 0.5: %1 %2 %3 %4")
                                .arg(red.x()).arg(red.y()).arg(red.z()).arg(red.w())));
        const QVector4D green = probe.colorAfterUpdate(QColor::fromRgbF(0, 1, 0, 1), 1.0);
        QVERIFY(close(green, QVector4D(0, 1, 0, 1)));
        // Полупрозрачный цвет: альфа цвета и непрозрачность узла - одно
        // произведение.
        const QVector4D marker = probe.colorAfterUpdate(QColor::fromRgbF(0, 1, 0, 0.4f), 0.5);
        QVERIFY2(close(marker, QVector4D(0, 0.2f, 0, 0.2f)),
                 qPrintable(QStringLiteral("маркер: %1 %2 %3 %4")
                                .arg(marker.x()).arg(marker.y()).arg(marker.z()).arg(marker.w())));
    }

    void createsShader()
    {
        InkMaterial material(InkMaterial::Layer::Wet);
        std::unique_ptr<QSGMaterialShader> shader(
            material.createShader(QSGRendererInterface::RenderMode2D));
        QVERIFY(shader);
    }
};

QTEST_MAIN(TestInkMaterial)
#include "tst_inkmaterial.moc"
