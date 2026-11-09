// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QQuickImageProvider>
#include <QImage>
#include <QHash>
#include <QMutex>

// Single-shot procedural wallpaper source. Qt Quick uploads the resulting
// QImage to a cached GPU texture; no per-frame CPU rendering is needed.
class WallpaperProvider final : public QQuickImageProvider {
public:
    WallpaperProvider();
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    // Exposed for deterministic scalar / SIMD regression tests.
    static QImage render(int width, int height, bool allowSimd = true);
    static bool avx2Available();

private:
    QMutex mutex_;
    QHash<QString, QImage> cache_;
};
