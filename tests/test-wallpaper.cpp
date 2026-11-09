// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/wallpaper.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <algorithm>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto start = QElapsedTimer();
    QElapsedTimer timer; timer.start();
    const QImage scalar = WallpaperProvider::render(257, 133, false);
    const qint64 scalarUs = timer.nsecsElapsed() / 1000;
    timer.restart();
    const QImage fast = WallpaperProvider::render(257, 133, true);
    const qint64 fastUs = timer.nsecsElapsed() / 1000;
    if (scalar.isNull() || fast.isNull() || scalar.size() != fast.size()) return 1;
    unsigned maxDiff = 0;
    for (int y = 0; y < scalar.height(); ++y) {
        for (int x = 0; x < scalar.width(); ++x) {
            const auto a = scalar.pixel(x, y), b = fast.pixel(x, y);
            maxDiff = std::max(maxDiff, static_cast<unsigned>(std::abs(qRed(a) - qRed(b))));
            maxDiff = std::max(maxDiff, static_cast<unsigned>(std::abs(qGreen(a) - qGreen(b))));
            maxDiff = std::max(maxDiff, static_cast<unsigned>(std::abs(qBlue(a) - qBlue(b))));
        }
    }
    qInfo() << "AVX2 available:" << WallpaperProvider::avx2Available()
            << "scalar usec:" << scalarUs << "dispatched usec:" << fastUs
            << "maximum channel delta:" << maxDiff;
    // SIMD uses rounded integer conversion; scalar uses lround. Allow one LSB.
    return maxDiff <= 1 ? 0 : 2;
}
