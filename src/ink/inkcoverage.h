#pragma once

#include <QPointF>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

#include "ink/stroketessellator.h"

// Покрытие кромки чернил на CPU - зеркало шейдеров src/canvas/shaders/ink.vert
// и ink.frag (design.md ink-beauty-pass, D7). GPU в тестах нет: паритет
// экспорта, пол толщины и стенд (вариант app-ink) считают покрытие здесь.
// Любая правка шейдера повторяется здесь, и наоборот.
namespace ink {

// Вынос вершины за кромку, device px: бахрома, в которой живёт сглаживание.
inline constexpr qreal kFringePx = 1.0;
// Пол полуширины, device px: самое тонкое перо на отдалённом зуме не
// распадается на точки. Значение - Open Questions design.md.
inline constexpr qreal kDefaultMinHalfWidthPx = 0.15;

struct DeviceVertex
{
    QPointF position;      // точки страницы, уже с выносом
    qreal distance = 0.0;  // d - знаковое расстояние поперёк штриха, device px
    qreal halfWidth = 0.0; // полуширина с полом, device px
};

// То, что делает ink.vert. pxPerUnit - device px в точке страницы. Вынос
// одинаков у всех вершин, и у внутренней вершины стыка тоже: предела по
// радиусу кривизны нет (D7).
inline DeviceVertex toDevice(const AxisVertex &v, qreal pxPerUnit,
                             qreal minHalfWidthPx = kDefaultMinHalfWidthPx)
{
    // Вынос задан в device px, а вершина - в точках страницы: без масштаба
    // перевести нечем.
    Q_ASSERT(pxPerUnit > 0.0);
    const qreal hwPx = std::max(qreal(v.hw) * pxPerUnit, minHalfWidthPx);
    const qreal extensionPx = hwPx + kFringePx;
    return {v.at(extensionPx / pxPerUnit), qreal(v.s) * extensionPx, hwPx};
}

// То, что делает ink.frag: длина пересечения [|d| − hw, |d| + hw] с пиксельным
// боксом [−0.5, 0.5]. При hw ≥ 0.5 - обычная кромка clamp(hw + 0.5 − |d|),
// у тонкой линии - трапеция с массой, равной толщине.
inline qreal coverage(qreal distancePx, qreal halfWidthPx)
{
    const qreal ad = std::abs(distancePx);
    return std::clamp(std::min(ad + halfWidthPx, 0.5) - std::max(ad - halfWidthPx, -0.5), 0.0,
                      1.0);
}

} // namespace ink
