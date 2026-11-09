// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime-dispatched AVX2 wallpaper. Scalar fallback for all CPUs.
// This code handles only non-sensitive pixels; the PAM worker never needs SIMD.
#include "wallpaper.h"
#include <QtGlobal>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdlib>

#ifndef PLM_ENABLE_SIMD
#define PLM_ENABLE_SIMD 1
#endif

#if PLM_ENABLE_SIMD && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define PLM_X86_AVX2 1
#include <immintrin.h>
#else
#define PLM_X86_AVX2 0
#endif

namespace {
struct Columns {
    std::vector<float> cyan, blue, purple, sun;
};

static Columns makeColumns(int width)
{
    Columns curves;
    curves.cyan.reserve(width);
    curves.blue.reserve(width);
    curves.purple.reserve(width);
    curves.sun.reserve(width);
    for (int x = 0; x < width; ++x) {
        const float u = static_cast<float>(x) / std::max(1, width - 1);
        // Shapes are evaluated once per column, not millions of times.
        curves.cyan.push_back(.85f - .56f * u +
                              .072f * std::sin(6.4f * u + .9f));
        curves.blue.push_back(.20f + .45f * u -
                              .082f * std::sin(5.3f * u + .35f));
        curves.purple.push_back(.95f - .29f * u +
                                .078f * std::sin(7.1f * u + 1.0f));
        const float d = (u - .60f) / .62f;
        curves.sun.push_back(1.f - d * d);
    }
    return curves;
}

static inline float peak(float distance, float inverseWidth)
{
    const float a = std::max(0.f, 1.f - std::abs(distance) * inverseWidth);
    return a * a;
}

static inline unsigned char clampByte(float x)
{
    return static_cast<unsigned char>(std::clamp(std::lround(x), 0L, 255L));
}

static QRgb scalarPixel(int x, float y, const Columns &c)
{
    const float cyan = peak(y - c.cyan[x], 3.8f);
    const float cyanEdge = peak(y - c.cyan[x], 60.f);
    const float blue = peak(y - c.blue[x], 4.2f);
    const float blueEdge = peak(y - c.blue[x], 63.f);
    const float purple = peak(y - c.purple[x], 5.0f);
    const float purpleEdge = peak(y - c.purple[x], 63.f);
    const float vd = (y - .47f) / .58f;
    const float sun = std::max(0.f, c.sun[x] - vd * vd);
    const float ambient = sun * sun;
    const float red = 5.f + 13.f * y + 20.f * cyan + 18.f * cyanEdge
                    + 21.f * blue + 21.f * blueEdge
                    + 92.f * purple + 60.f * purpleEdge + 24.f * ambient;
    const float green = 12.f + 14.f * y + 78.f * cyan + 96.f * cyanEdge
                    + 47.f * blue + 57.f * blueEdge
                    + 33.f * purple + 18.f * purpleEdge + 27.f * ambient;
    const float b = 35.f + 26.f * y + 144.f * cyan + 98.f * cyanEdge
                    + 146.f * blue + 104.f * blueEdge
                    + 142.f * purple + 65.f * purpleEdge + 71.f * ambient;
    return qRgb(clampByte(red), clampByte(green), clampByte(b));
}

static void renderScalar(QImage &image, const Columns &c, int startX = 0)
{
    for (int y = 0; y < image.height(); ++y) {
        const float v = static_cast<float>(y) / std::max(1, image.height() - 1);
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = startX; x < image.width(); ++x)
            row[x] = scalarPixel(x, v, c);
    }
}

#if PLM_X86_AVX2
__attribute__((target("avx2")))
static inline __m256 peak8(__m256 y, __m256 center, float scale)
{
    const __m256 distance = _mm256_sub_ps(y, center);
    const __m256 absDistance = _mm256_andnot_ps(
        _mm256_set1_ps(-0.f), distance);
    const __m256 t = _mm256_max_ps(_mm256_setzero_ps(),
        _mm256_sub_ps(_mm256_set1_ps(1.f),
                      _mm256_mul_ps(absDistance, _mm256_set1_ps(scale))));
    return _mm256_mul_ps(t, t);
}

__attribute__((target("avx2")))
static inline __m256 addWeighted(__m256 base, __m256 part, float weight)
{
    return _mm256_add_ps(base,
                        _mm256_mul_ps(part, _mm256_set1_ps(weight)));
}

__attribute__((target("avx2")))
static void renderAvx2(QImage &image, const Columns &c)
{
    const int w = image.width();
    const int groups = (w / 8) * 8;
    const __m256 zero = _mm256_setzero_ps();
    const __m256 upper = _mm256_set1_ps(255.f);
    for (int y = 0; y < image.height(); ++y) {
        const float v = static_cast<float>(y) / std::max(1, image.height() - 1);
        const __m256 vv = _mm256_set1_ps(v);
        const float vd = (v - .47f) / .58f;
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < groups; x += 8) {
            const __m256 cyan = _mm256_loadu_ps(c.cyan.data() + x);
            const __m256 blue = _mm256_loadu_ps(c.blue.data() + x);
            const __m256 purple = _mm256_loadu_ps(c.purple.data() + x);
            const __m256 cp = peak8(vv, cyan, 3.8f);
            const __m256 ce = peak8(vv, cyan, 60.f);
            const __m256 bp = peak8(vv, blue, 4.2f);
            const __m256 be = peak8(vv, blue, 63.f);
            const __m256 pp = peak8(vv, purple, 5.0f);
            const __m256 pe = peak8(vv, purple, 63.f);
            const __m256 sun = _mm256_max_ps(zero,
                _mm256_sub_ps(_mm256_loadu_ps(c.sun.data() + x),
                              _mm256_set1_ps(vd * vd)));
            const __m256 ambient = _mm256_mul_ps(sun, sun);
            __m256 r = _mm256_set1_ps(5.f + 13.f * v);
            r = addWeighted(r, cp, 20.f); r = addWeighted(r, ce, 18.f);
            r = addWeighted(r, bp, 21.f); r = addWeighted(r, be, 21.f);
            r = addWeighted(r, pp, 92.f); r = addWeighted(r, pe, 60.f);
            r = addWeighted(r, ambient, 24.f);
            __m256 g = _mm256_set1_ps(12.f + 14.f * v);
            g = addWeighted(g, cp, 78.f); g = addWeighted(g, ce, 96.f);
            g = addWeighted(g, bp, 47.f); g = addWeighted(g, be, 57.f);
            g = addWeighted(g, pp, 33.f); g = addWeighted(g, pe, 18.f);
            g = addWeighted(g, ambient, 27.f);
            __m256 b = _mm256_set1_ps(35.f + 26.f * v);
            b = addWeighted(b, cp, 144.f); b = addWeighted(b, ce, 98.f);
            b = addWeighted(b, bp, 146.f); b = addWeighted(b, be, 104.f);
            b = addWeighted(b, pp, 142.f); b = addWeighted(b, pe, 65.f);
            b = addWeighted(b, ambient, 71.f);
            const __m256i r8 = _mm256_cvtps_epi32(_mm256_min_ps(upper, r));
            const __m256i g8 = _mm256_cvtps_epi32(_mm256_min_ps(upper, g));
            const __m256i b8 = _mm256_cvtps_epi32(_mm256_min_ps(upper, b));
            const __m256i pixel = _mm256_or_si256(
                _mm256_set1_epi32(static_cast<int>(0xff000000u)),
                _mm256_or_si256(
                    _mm256_slli_epi32(r8, 16),
                    _mm256_or_si256(_mm256_slli_epi32(g8, 8), b8)));
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(row + x), pixel);
        }
        for (int x = groups; x < w; ++x)
            row[x] = scalarPixel(x, v, c);
    }
}
#endif
} // namespace

WallpaperProvider::WallpaperProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

bool WallpaperProvider::avx2Available()
{
#if PLM_X86_AVX2
    static const bool available = __builtin_cpu_supports("avx2") != 0;
    return available;
#else
    return false;
#endif
}

QImage WallpaperProvider::render(int width, int height, bool allowSimd)
{
    width = std::clamp(width, 64, 2560);
    height = std::clamp(height, 64, 1440);
    QImage image(width, height, QImage::Format_RGB32);
    if (image.isNull()) return image;
    const Columns curves = makeColumns(width);
#if PLM_X86_AVX2
    if (allowSimd && avx2Available() && !qEnvironmentVariableIsSet("PLM_DISABLE_SIMD"))
        renderAvx2(image, curves);
    else
        renderScalar(image, curves);
#else
    (void)allowSimd;
    renderScalar(image, curves);
#endif
    return image;
}

QImage WallpaperProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    int w = 1600, h = 900;
    const QStringList parts = id.split('x');
    if (parts.size() == 2) {
        bool okW = false, okH = false;
        const int parsedW = parts[0].toInt(&okW);
        const int parsedH = parts[1].toInt(&okH);
        if (okW && okH) { w = parsedW; h = parsedH; }
    }
    if (requestedSize.isValid()) { w = requestedSize.width(); h = requestedSize.height(); }
    w = std::clamp(w, 64, 2560);
    h = std::clamp(h, 64, 1440);
    if (size) *size = QSize(w, h);
    const QString key = QString::number(w) + "x" + QString::number(h);
    QMutexLocker lock(&mutex_);
    auto found = cache_.constFind(key);
    if (found != cache_.constEnd()) return found.value();
    QImage result = render(w, h);
    if (cache_.size() >= 2) cache_.clear();
    cache_.insert(key, result);
    return result;
}
