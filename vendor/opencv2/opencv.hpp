#pragma once
#include <stdint.h>
#include <stddef.h>

#define CV_8UC4 0

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
    Mat() : data(nullptr), step(0) {}
    Mat(int rows, int cols, int type) : data(nullptr), step(0) {}
    Mat(int rows, int cols, int type, void* _data, size_t _step = 0) : data(_data), step((int)_step) {}
    uint8_t* ptr(int) { return (uint8_t*)data; }
    int step1(int) { return step; }
};

inline void boxFilter(Mat, Mat, int, Size) {}
inline void resize(Mat, Mat, Size, double, double, int) {}
inline Mat getAffineTransform(const Point2f[], const Point2f[]) { return Mat(); }
inline void warpAffine(Mat, Mat, Mat, Size, int) {}
inline Mat getPerspectiveTransform(const Point2f[], const Point2f[]) { return Mat(); }
inline void warpPerspective(Mat, Mat, Mat, Size, int) {}

const int INTER_NEAREST = 0;
const int INTER_LINEAR = 1;
const int INTER_CUBIC = 2;
const int INTER_AREA = 3;

}
