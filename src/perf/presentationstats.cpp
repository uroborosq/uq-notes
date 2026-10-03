#include "presentationstats.h"

#include <algorithm>

namespace {

// Тот же порядок статистики, что у интервалов кадров в FrameStats: медиана -
// средний элемент отсортированной выборки, p95 - элемент с индексом n·0.95.
qreal sortedAt(const QVector<qreal> &sorted, qreal quantile)
{
    if (sorted.isEmpty())
        return 0.0;
    const qsizetype index = std::min(sorted.size() - 1, qsizetype(sorted.size() * quantile));
    return sorted[index];
}

} // namespace

void FrameInputs::add(qint64 eventNs)
{
    if (eventNs <= 0)
        return;
    if (oldestNs <= 0 || eventNs < oldestNs)
        oldestNs = eventNs;
    newestNs = std::max(newestNs, eventNs);
}

void FrameInputTracker::noteInput(qint64 eventNs)
{
    if (eventNs <= 0)
        return;
    qint64 expected = 0;
    m_pendingOldestNs.compare_exchange_strong(expected, eventNs, std::memory_order_relaxed);
    m_pendingNewestNs.store(eventNs, std::memory_order_relaxed);
}

void FrameInputTracker::beginFrame()
{
    // Поток GUI стоит на синхронизации, поэтому всё, что он успел отметить,
    // войдёт ровно в этот кадр, а отмеченное позже - в следующий.
    m_frame = FrameInputs();
    m_frame.add(m_pendingOldestNs.exchange(0, std::memory_order_relaxed));
    m_frame.add(m_pendingNewestNs.exchange(0, std::memory_order_relaxed));
}

FrameInputs FrameInputTracker::takeForRenderedFrame()
{
    const FrameInputs frame = m_frame;
    m_frame = FrameInputs();
    return frame;
}

void PresentationStats::addPresented(const FrameInputs &inputs, qint64 presentedNs)
{
    if (inputs.isEmpty())
        return;
    // Показ раньше события физически невозможен: значит, метка события или
    // время показа пришли не в CLOCK_MONOTONIC. Такой кадр не смешивается со
    // сводкой, а считается отдельно - по счётчику видно, что замер врёт.
    if (presentedNs < inputs.newestNs) {
        ++m_rejected;
        return;
    }
    m_oldestMs.append((presentedNs - inputs.oldestNs) / 1e6);
    m_newestMs.append((presentedNs - inputs.newestNs) / 1e6);
}

PresentationSummary PresentationStats::takeSummary()
{
    PresentationSummary summary;
    summary.presented = int(m_oldestMs.size());
    summary.discarded = m_discarded;
    summary.rejected = m_rejected;
    std::sort(m_oldestMs.begin(), m_oldestMs.end());
    std::sort(m_newestMs.begin(), m_newestMs.end());
    summary.oldestMedianMs = sortedAt(m_oldestMs, 0.5);
    summary.oldestP95Ms = sortedAt(m_oldestMs, 0.95);
    summary.newestMedianMs = sortedAt(m_newestMs, 0.5);
    summary.maxMs = m_oldestMs.isEmpty() ? 0.0 : m_oldestMs.constLast();
    m_oldestMs.clear();
    m_newestMs.clear();
    m_discarded = 0;
    m_rejected = 0;
    return summary;
}

QString PresentationSummary::toString() const
{
    return QStringLiteral("event->present: frames %1 | oldest median %2 p95 %3 | newest median %4"
                          " | max %5 ms | discarded %6 | clock mismatch %7")
        .arg(presented)
        .arg(oldestMedianMs, 0, 'f', 1)
        .arg(oldestP95Ms, 0, 'f', 1)
        .arg(newestMedianMs, 0, 'f', 1)
        .arg(maxMs, 0, 'f', 1)
        .arg(discarded)
        .arg(rejected);
}
