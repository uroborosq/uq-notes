#include "surfaceformat.h"

namespace {

constexpr int kDefaultSamples = 0;

} // namespace

int surfaceSampleCount(const QByteArray &environmentValue)
{
    bool ok = false;
    const int samples = environmentValue.trimmed().toInt(&ok);
    if (!ok)
        return kDefaultSamples;
    switch (samples) {
    case 0:
    case 2:
    case 4:
    case 8:
    case 16:
        return samples;
    default:
        return kDefaultSamples;
    }
}
