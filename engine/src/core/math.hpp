#pragma once
// Minimal column-major 4x4 matrix math (OpenGL convention).

#include <cmath>

namespace cr {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 perspective(float fovyRad, float aspect, float zNear, float zFar) {
        Mat4 r;
        const float f = 1.0f / std::tan(fovyRad / 2);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zFar + zNear) / (zNear - zFar);
        r.m[11] = -1;
        r.m[14] = 2 * zFar * zNear / (zNear - zFar);
        r.m[15] = 0;
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
        const Vec3 f = normalize(target - eye);
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        return r;
    }

    // Maps model Z-up to Y-up (rotation of -90 degrees about X).
    static Mat4 zUpToYUp() {
        Mat4 r;
        r.m[5] = 0; r.m[6] = -1;
        r.m[9] = 1; r.m[10] = 0;
        return r;
    }

    // Inverse of an affine matrix (general 3x3 part + translation).
    Mat4 affineInverse() const {
        const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        Mat4 r;
        if (std::fabs(det) < 1e-12f) return r;
        const float inv = 1.0f / det;
        r.m[0] = A * inv;                  r.m[4] = -(b * i - c * h) * inv;  r.m[8] = (b * f - c * e) * inv;
        r.m[1] = B * inv;                  r.m[5] = (a * i - c * g) * inv;   r.m[9] = -(a * f - c * d) * inv;
        r.m[2] = C * inv;                  r.m[6] = -(a * h - b * g) * inv;  r.m[10] = (a * e - b * d) * inv;
        const float tx = m[12], ty = m[13], tz = m[14];
        r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
        r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
        r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
        return r;
    }

    Vec3 transformPoint(const float* p) const {
        return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
};

} // namespace cr
