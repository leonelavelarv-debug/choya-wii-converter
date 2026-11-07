// Cámara orbital alrededor de la caja del modelo (matrices columna-mayor 4x4).
#pragma once
#include <algorithm>
#include <cmath>

#include "wl/model.h"

namespace wl {

struct Orbit {
    float yaw = 0.6f, pitch = 0.35f, zoom = 1.0f;
};

inline void perspective(float fovy, float aspect, float zn, float zf, float out[16]) {
    const float f = 1.0f / std::tan(fovy / 2);
    for (int i = 0; i < 16; ++i) out[i] = 0;
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (zf + zn) / (zn - zf);
    out[11] = -1;
    out[14] = 2 * zf * zn / (zn - zf);
}

inline void look_at(const float eye[3], const float at[3], float r[16]) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    float s[3] = {-f[2], 0, f[0]};  // f x (0,1,0)
    float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    if (sl < 1e-6f) {
        s[0] = 1;
        sl = 1;
    }
    for (float& v : s) v /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    for (int i = 0; i < 16; ++i) r[i] = 0;
    r[0] = s[0]; r[4] = s[1]; r[8] = s[2];
    r[1] = u[0]; r[5] = u[1]; r[9] = u[2];
    r[2] = -f[0]; r[6] = -f[1]; r[10] = -f[2];
    r[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    r[15] = 1;
}

// Matrices de vista y proyección para ver todo el modelo desde la órbita dada.
inline void orbit_matrices(const ModelView& v, const Orbit& o, float aspect, float view[16], float proj[16]) {
    float c[3], ext = 0;
    for (int k = 0; k < 3; ++k) {
        c[k] = (v.bbox_min[k] + v.bbox_max[k]) / 2;
        ext = std::max(ext, v.bbox_max[k] - v.bbox_min[k]);
    }
    const float radius = std::max(ext * 0.5f, 0.01f);
    const float dist = radius * 2.6f * o.zoom;
    const float eye[3] = {c[0] + dist * std::cos(o.pitch) * std::sin(o.yaw), c[1] + dist * std::sin(o.pitch),
                          c[2] + dist * std::cos(o.pitch) * std::cos(o.yaw)};
    look_at(eye, c, view);
    perspective(0.9f, aspect, dist * 0.01f, dist * 10 + radius * 4, proj);
}

}  // namespace wl
