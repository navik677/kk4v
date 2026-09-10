#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CV_8UC4 0

// Minimal drop-in replacement for the small subset of OpenCV used by
// RenderManager.cpp (resize / warpAffine / warpPerspective / boxFilter)
// on platforms (like the Vita) where real OpenCV isn't available.
//
// All buffers are treated as tightly-packed-per-row RGBA8888 (CV_8UC4),
// which matches every call site in this codebase.
//
// IMPORTANT: the original version of this header had every function as
// an empty no-op and getAffineTransform/getPerspectiveTransform return
// a default-constructed (data=nullptr) Mat. Callers pass a *default
// constructed* `dst` Mat expecting resize/warpAffine/warpPerspective to
// allocate it (exactly like real OpenCV does), then immediately read
// dst.ptr(0)/dst.step1(0) and hand that pointer to a texture that gets
// rendered from. With the no-op stubs, that pointer was always null,
// so any code path exercising a perspective/affine quad draw (used by
// this engine's KAG button hover/click transform effects) was a
// guaranteed null-pointer dereference a few frames later during
// compositing.

namespace cv {

struct Size {
    int width, height;
    Size() : width(0), height(0) {}
    Size(int w, int h) : width(w), height(h) {}
};

struct Point2f {
    float x, y;
    Point2f() : x(0), y(0) {}
    Point2f(float _x, float _y) : x(_x), y(_y) {}
};

struct Mat {
    void* data;
    int step;
    int rows, cols;
    bool owns; // true if `data` was heap-allocated by us and must be freed
    Mat() : data(nullptr), step(0), rows(0), cols(0), owns(false) {}
    Mat(int _rows, int _cols, int /*type*/) : data(nullptr), step(0), rows(_rows), cols(_cols), owns(false) {}
    Mat(int _rows, int _cols, int /*type*/, void* _data, size_t _step = 0)
        : data(_data), step(_step ? (int)_step : _cols * 4), rows(_rows), cols(_cols), owns(false) {}

    uint8_t* ptr(int y = 0) { return (uint8_t*)data + (size_t)y * step; }
    const uint8_t* ptr(int y = 0) const { return (const uint8_t*)data + (size_t)y * step; }
    int step1(int /*i*/ = 0) const { return step; }

    void allocate(int _rows, int _cols) {
        rows = _rows; cols = _cols;
        step = _cols * 4;
        data = malloc((size_t)step * (size_t)rows);
        owns = true;
    }
};

inline void ensureDst(Mat &dst, const Size &sz) {
    if (!dst.data || dst.rows != sz.height || dst.cols != sz.width)
        dst.allocate(sz.height, sz.width);
}

inline uint32_t samplePixelNearest(const Mat &src, float x, float y) {
    int ix = (int)(x + 0.5f), iy = (int)(y + 0.5f);
    if (ix < 0 || iy < 0 || ix >= src.cols || iy >= src.rows) return 0;
    return *(const uint32_t*)(src.ptr(iy) + ix * 4);
}

// resize()/boxFilter() are left as no-ops like the original stub: both of
// this file's call sites already pass a `dst` Mat backed by a real,
// pre-allocated buffer (it wraps an existing texture's scanline), so unlike
// warpPerspective below there's no null-pointer risk here -- just a cosmetic
// one (stretched/blurred content won't visually update).
inline void resize(const Mat &, Mat &, Size, double, double, int) {}
inline void boxFilter(const Mat &, Mat &, int, Size) {}

// Solve A*x = b for a small (n<=8) linear system via Gaussian elimination.
inline bool solveLinear(double *A, double *b, int n) {
    for (int i = 0; i < n; ++i) {
        int piv = i;
        for (int r = i + 1; r < n; ++r)
            if (fabs(A[r * n + i]) > fabs(A[piv * n + i])) piv = r;
        if (fabs(A[piv * n + i]) < 1e-12) return false;
        if (piv != i) {
            for (int c = 0; c < n; ++c) { double t = A[i * n + c]; A[i * n + c] = A[piv * n + c]; A[piv * n + c] = t; }
            double t = b[i]; b[i] = b[piv]; b[piv] = t;
        }
        for (int r = 0; r < n; ++r) {
            if (r == i) continue;
            double f = A[r * n + i] / A[i * n + i];
            for (int c = i; c < n; ++c) A[r * n + c] -= f * A[i * n + c];
            b[r] -= f * b[i];
        }
    }
    for (int i = 0; i < n; ++i) b[i] /= A[i * n + i];
    return true;
}

struct Mat3x3 { double m[9]; bool valid; };

// getAffineTransform/warpAffine are intentionally not implemented: the only
// caller in this codebase is compiled out (`#ifdef USE_CV_AFFINE`, which is
// never defined -- see RenderManager.cpp), so this engine always takes the
// warpPerspective path below for quad draws.

inline Mat getPerspectiveTransform(const Point2f src[4], const Point2f dst[4]) {
    // Standard 4-point homography solve (8 unknowns, h33 fixed to 1).
    double A[64] = {0}, b[8];
    for (int i = 0; i < 4; ++i) {
        double sx = src[i].x, sy = src[i].y, dx = dst[i].x, dy = dst[i].y;
        double *r0 = A + (i * 2) * 8;
        r0[0] = sx; r0[1] = sy; r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0; r0[6] = -sx * dx; r0[7] = -sy * dx;
        b[i * 2] = dx;
        double *r1 = A + (i * 2 + 1) * 8;
        r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = sx; r1[4] = sy; r1[5] = 1; r1[6] = -sx * dy; r1[7] = -sy * dy;
        b[i * 2 + 1] = dy;
    }
    Mat3x3 *r = new Mat3x3();
    bool ok = solveLinear(A, b, 8);
    r->valid = ok;
    if (ok) { for (int i = 0; i < 8; ++i) r->m[i] = b[i]; r->m[8] = 1.0; }
    Mat m;
    m.data = r;
    m.step = 0;
    m.rows = 3; m.cols = 3;
    m.owns = false;
    return m;
}

inline void warpPerspective(const Mat &src, Mat &dst, const Mat &M, Size dsize, int /*flags*/) {
    if (dsize.width <= 0 || dsize.height <= 0) return;
    ensureDst(dst, dsize);
    memset(dst.data, 0, (size_t)dst.step * dst.rows);
    Mat3x3 *mm = (Mat3x3*)M.data;
    if (!mm || !mm->valid) { delete mm; return; }
    double h[9];
    for (int i = 0; i < 9; ++i) h[i] = mm->m[i];
    delete mm;
    // Invert the 3x3 homography (forward src -> dst) for dst -> src sampling.
    double a = h[0], b_ = h[1], c = h[2];
    double d = h[3], e = h[4], f = h[5];
    double g = h[6], hh = h[7], ii = h[8];
    double det = a * (e * ii - f * hh) - b_ * (d * ii - f * g) + c * (d * hh - e * g);
    if (fabs(det) < 1e-12) return;
    double inv[9] = {
        (e * ii - f * hh) / det, (c * hh - b_ * ii) / det, (b_ * f - c * e) / det,
        (f * g - d * ii) / det,  (a * ii - c * g) / det,   (c * d - a * f) / det,
        (d * hh - e * g) / det,  (b_ * g - a * hh) / det,  (a * e - b_ * d) / det
    };
    for (int y = 0; y < dsize.height; ++y) {
        uint32_t *drow = (uint32_t*)dst.ptr(y);
        for (int x = 0; x < dsize.width; ++x) {
            double w = inv[6] * x + inv[7] * y + inv[8];
            if (fabs(w) < 1e-12) { drow[x] = 0; continue; }
            double sx = (inv[0] * x + inv[1] * y + inv[2]) / w;
            double sy = (inv[3] * x + inv[4] * y + inv[5]) / w;
            drow[x] = samplePixelNearest(src, (float)sx, (float)sy);
        }
    }
}

const int INTER_NEAREST = 0;
const int INTER_LINEAR = 1;
const int INTER_CUBIC = 2;
const int INTER_AREA = 3;

}
