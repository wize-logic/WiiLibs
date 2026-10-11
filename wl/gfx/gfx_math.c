// SPDX-License-Identifier: MIT
// Matrix and camera maths (gfx_math.h).
#include "wl/gfx/gfx_math.h"

#include <math.h>
#include <string.h>

#define WL_PI 3.14159265358979f

WlVec3 wl_vec3(float x, float y, float z) {
    WlVec3 v = {x, y, z};
    return v;
}

WlVec3 wl_vec3_add(WlVec3 a, WlVec3 b) {
    return wl_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

WlVec3 wl_vec3_sub(WlVec3 a, WlVec3 b) {
    return wl_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

WlVec3 wl_vec3_scale(WlVec3 a, float s) {
    return wl_vec3(a.x * s, a.y * s, a.z * s);
}

float wl_vec3_dot(WlVec3 a, WlVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

WlVec3 wl_vec3_cross(WlVec3 a, WlVec3 b) {
    return wl_vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

float wl_vec3_length(WlVec3 a) {
    return sqrtf(wl_vec3_dot(a, a));
}

WlVec3 wl_vec3_normalize(WlVec3 a) {
    float len = wl_vec3_length(a);
    return len > 0.0f ? wl_vec3_scale(a, 1.0f / len) : a;
}

void wl_mat34_identity(WlMat34 *out) {
    static const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(out->m, id, sizeof id);
}

void wl_mat34_mul(WlMat34 *out, const WlMat34 *a, const WlMat34 *b) {
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float v = a->m[i * 4 + 0] * b->m[0 * 4 + j] + a->m[i * 4 + 1] * b->m[1 * 4 + j] +
                      a->m[i * 4 + 2] * b->m[2 * 4 + j];
            if (j == 3) {
                v += a->m[i * 4 + 3];
            }
            r[i * 4 + j] = v;
        }
    }
    memcpy(out->m, r, sizeof r);
}

void wl_mat34_translate(WlMat34 *out, float x, float y, float z) {
    wl_mat34_identity(out);
    out->m[3] = x;
    out->m[7] = y;
    out->m[11] = z;
}

void wl_mat34_scale(WlMat34 *out, float x, float y, float z) {
    wl_mat34_identity(out);
    out->m[0] = x;
    out->m[5] = y;
    out->m[10] = z;
}

void wl_mat34_rotate_x(WlMat34 *out, float r) {
    float c = cosf(r);
    float s = sinf(r);
    wl_mat34_identity(out);
    out->m[5] = c;
    out->m[6] = -s;
    out->m[9] = s;
    out->m[10] = c;
}

void wl_mat34_rotate_y(WlMat34 *out, float r) {
    float c = cosf(r);
    float s = sinf(r);
    wl_mat34_identity(out);
    out->m[0] = c;
    out->m[2] = s;
    out->m[8] = -s;
    out->m[10] = c;
}

void wl_mat34_rotate_z(WlMat34 *out, float r) {
    float c = cosf(r);
    float s = sinf(r);
    wl_mat34_identity(out);
    out->m[0] = c;
    out->m[1] = -s;
    out->m[4] = s;
    out->m[5] = c;
}

void wl_mat34_rotate_axis(WlMat34 *out, WlVec3 axis, float r) {
    WlVec3 a = wl_vec3_normalize(axis);
    float c = cosf(r);
    float s = sinf(r);
    float t = 1.0f - c;
    wl_mat34_identity(out);
    out->m[0] = t * a.x * a.x + c;
    out->m[1] = t * a.x * a.y - s * a.z;
    out->m[2] = t * a.x * a.z + s * a.y;
    out->m[4] = t * a.x * a.y + s * a.z;
    out->m[5] = t * a.y * a.y + c;
    out->m[6] = t * a.y * a.z - s * a.x;
    out->m[8] = t * a.x * a.z - s * a.y;
    out->m[9] = t * a.y * a.z + s * a.x;
    out->m[10] = t * a.z * a.z + c;
}

void wl_mat34_from_trs(WlMat34 *out, const float t[3], const float q[4], const float s[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n2 = x * x + y * y + z * z + w * w;
    if (!(n2 > 1e-20f)) {
        x = y = z = 0.0f;
        w = 1.0f;
        n2 = 1.0f;
    }
    float k = 2.0f / n2; // 2 / |q|^2 folds the normalisation into the products
    float xx = x * x * k, yy = y * y * k, zz = z * z * k;
    float xy = x * y * k, xz = x * z * k, yz = y * z * k;
    float wx = w * x * k, wy = w * y * k, wz = w * z * k;
    float sx = s != NULL ? s[0] : 1.0f, sy = s != NULL ? s[1] : 1.0f, sz = s != NULL ? s[2] : 1.0f;
    float *m = out->m;
    m[0] = (1.0f - yy - zz) * sx;
    m[1] = (xy - wz) * sy;
    m[2] = (xz + wy) * sz;
    m[3] = t[0];
    m[4] = (xy + wz) * sx;
    m[5] = (1.0f - xx - zz) * sy;
    m[6] = (yz - wx) * sz;
    m[7] = t[1];
    m[8] = (xz - wy) * sx;
    m[9] = (yz + wx) * sy;
    m[10] = (1.0f - xx - yy) * sz;
    m[11] = t[2];
}

bool wl_mat34_inverse(WlMat34 *out, const WlMat34 *in) {
    const float *m = in->m;
    float c0 = m[5] * m[10] - m[6] * m[9];
    float c1 = m[6] * m[8] - m[4] * m[10];
    float c2 = m[4] * m[9] - m[5] * m[8];
    float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (fabsf(det) < 1e-12f) {
        return false;
    }
    float id = 1.0f / det;
    float r[12];
    r[0] = c0 * id;
    r[1] = (m[2] * m[9] - m[1] * m[10]) * id;
    r[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    r[4] = c1 * id;
    r[5] = (m[0] * m[10] - m[2] * m[8]) * id;
    r[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    r[8] = c2 * id;
    r[9] = (m[1] * m[8] - m[0] * m[9]) * id;
    r[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    r[3] = -(r[0] * m[3] + r[1] * m[7] + r[2] * m[11]);
    r[7] = -(r[4] * m[3] + r[5] * m[7] + r[6] * m[11]);
    r[11] = -(r[8] * m[3] + r[9] * m[7] + r[10] * m[11]);
    memcpy(out->m, r, sizeof r);
    return true;
}

WlVec3 wl_mat34_transform_point(const WlMat34 *m, WlVec3 p) {
    const float *a = m->m;
    return wl_vec3(a[0] * p.x + a[1] * p.y + a[2] * p.z + a[3], a[4] * p.x + a[5] * p.y + a[6] * p.z + a[7],
                   a[8] * p.x + a[9] * p.y + a[10] * p.z + a[11]);
}

WlVec3 wl_mat34_transform_dir(const WlMat34 *m, WlVec3 d) {
    const float *a = m->m;
    return wl_vec3(a[0] * d.x + a[1] * d.y + a[2] * d.z, a[4] * d.x + a[5] * d.y + a[6] * d.z,
                   a[8] * d.x + a[9] * d.y + a[10] * d.z);
}

void wl_mat34_look_at(WlMat34 *out, WlVec3 eye, WlVec3 target, WlVec3 up) {
    WlVec3 z = wl_vec3_normalize(wl_vec3_sub(eye, target)); // the camera's +Z points backwards
    WlVec3 x = wl_vec3_normalize(wl_vec3_cross(up, z));
    if (wl_vec3_length(x) == 0.0f) { // looking along `up`: any perpendicular will do
        x = wl_vec3_normalize(wl_vec3_cross(wl_vec3(1.0f, 0.0f, 0.0f), z));
        if (wl_vec3_length(x) == 0.0f) {
            x = wl_vec3_normalize(wl_vec3_cross(wl_vec3(0.0f, 0.0f, 1.0f), z));
        }
    }
    WlVec3 y = wl_vec3_cross(z, x);
    out->m[0] = x.x;
    out->m[1] = x.y;
    out->m[2] = x.z;
    out->m[3] = -wl_vec3_dot(x, eye);
    out->m[4] = y.x;
    out->m[5] = y.y;
    out->m[6] = y.z;
    out->m[7] = -wl_vec3_dot(y, eye);
    out->m[8] = z.x;
    out->m[9] = z.y;
    out->m[10] = z.z;
    out->m[11] = -wl_vec3_dot(z, eye);
}

void wl_mat44_perspective(WlMat44 *out, float fovy, float aspect, float n, float f) {
    float cot = 1.0f / tanf(fovy * (WL_PI / 180.0f) * 0.5f);
    memset(out->m, 0, sizeof out->m);
    out->m[0] = cot / aspect;
    out->m[5] = cot;
    out->m[10] = -n / (f - n);
    out->m[11] = -(f * n) / (f - n);
    out->m[14] = -1.0f;
}

void wl_mat44_ortho(WlMat44 *out, float t, float b, float l, float r, float n, float f) {
    memset(out->m, 0, sizeof out->m);
    out->m[0] = 2.0f / (r - l);
    out->m[3] = -(r + l) / (r - l);
    out->m[5] = 2.0f / (t - b);
    out->m[7] = -(t + b) / (t - b);
    out->m[10] = -1.0f / (f - n);
    out->m[11] = -f / (f - n);
    out->m[15] = 1.0f;
}

// ---- cameras ----------------------------------------------------------------------------------

void wl_camera_default(WlCamera *cam, float aspect) {
    memset(cam, 0, sizeof *cam);
    cam->eye = wl_vec3(0.0f, 0.0f, 10.0f);
    cam->target = wl_vec3(0.0f, 0.0f, 0.0f);
    cam->up = wl_vec3(0.0f, 1.0f, 0.0f);
    cam->fovy_degrees = 45.0f;
    cam->aspect = aspect;
    cam->znear = 0.5f;
    cam->zfar = 500.0f;
    cam->ortho_height = 10.0f;
}

void wl_camera_view(const WlCamera *cam, WlMat34 *out) {
    wl_mat34_look_at(out, cam->eye, cam->target, cam->up);
}

void wl_camera_orbit(WlCamera *cam, WlVec3 target, float yaw, float pitch, float dist) {
    float cp = cosf(pitch);
    cam->target = target;
    cam->eye = wl_vec3(target.x + dist * cp * sinf(yaw), target.y + dist * sinf(pitch),
                       target.z + dist * cp * cosf(yaw));
}

void wl_camera_projection(const WlCamera *cam, WlMat44 *out) {
    if (cam->ortho) {
        float hh = cam->ortho_height * 0.5f;
        float hw = hh * cam->aspect;
        wl_mat44_ortho(out, hh, -hh, -hw, hw, cam->znear, cam->zfar);
    } else {
        wl_mat44_perspective(out, cam->fovy_degrees, cam->aspect, cam->znear, cam->zfar);
    }
}

bool wl_camera_project(const WlCamera *cam, WlVec3 world, float vw, float vh, float out[3]) {
    WlMat34 view;
    wl_camera_view(cam, &view);
    WlVec3 v = wl_mat34_transform_point(&view, world);
    float dist = -v.z; // distance in front of the camera
    float nx;
    float ny;
    if (cam->ortho) {
        float hh = cam->ortho_height * 0.5f;
        nx = v.x / (hh * cam->aspect);
        ny = v.y / hh;
    } else {
        float t = tanf(cam->fovy_degrees * (WL_PI / 180.0f) * 0.5f);
        float d = dist != 0.0f ? dist : 1e-9f;
        nx = v.x / (d * t * cam->aspect);
        ny = v.y / (d * t);
    }
    out[0] = (nx * 0.5f + 0.5f) * vw;
    out[1] = (0.5f - ny * 0.5f) * vh;
    out[2] = dist;
    return dist > 0.0f;
}

void wl_camera_frustum(const WlCamera *cam, WlFrustum *out) {
    WlMat34 view;
    wl_camera_view(cam, &view);
    // Planes in view space (inside: n.p + d >= 0), then carried to world space: n_w = R^T n_v, d_w = n_v.t + d.
    float v[6][4];
    if (cam->ortho) {
        float hh = cam->ortho_height * 0.5f;
        float hw = hh * cam->aspect;
        float t[6][4] = {{1, 0, 0, hw}, {-1, 0, 0, hw}, {0, 1, 0, hh}, {0, -1, 0, hh}, {0, 0, -1, -cam->znear},
                         {0, 0, 1, cam->zfar}};
        memcpy(v, t, sizeof v);
    } else {
        float ty = tanf(cam->fovy_degrees * (WL_PI / 180.0f) * 0.5f);
        float tx = ty * cam->aspect;
        float t[6][4] = {{1, 0, -tx, 0}, {-1, 0, -tx, 0}, {0, 1, -ty, 0}, {0, -1, -ty, 0}, {0, 0, -1, -cam->znear},
                         {0, 0, 1, cam->zfar}};
        memcpy(v, t, sizeof v);
    }
    const float *m = view.m;
    for (int i = 0; i < 6; i++) {
        float nx = m[0] * v[i][0] + m[4] * v[i][1] + m[8] * v[i][2];
        float ny = m[1] * v[i][0] + m[5] * v[i][1] + m[9] * v[i][2];
        float nz = m[2] * v[i][0] + m[6] * v[i][1] + m[10] * v[i][2];
        float d = v[i][0] * m[3] + v[i][1] * m[7] + v[i][2] * m[11] + v[i][3];
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        float il = len > 0.0f ? 1.0f / len : 1.0f;
        out->p[i][0] = nx * il;
        out->p[i][1] = ny * il;
        out->p[i][2] = nz * il;
        out->p[i][3] = d * il;
    }
}

bool wl_frustum_sphere(const WlFrustum *f, WlVec3 c, float r) {
    for (int i = 0; i < 6; i++) {
        if (f->p[i][0] * c.x + f->p[i][1] * c.y + f->p[i][2] * c.z + f->p[i][3] < -r) {
            return false;
        }
    }
    return true;
}

bool wl_frustum_aabb(const WlFrustum *f, WlVec3 mn, WlVec3 mx) {
    for (int i = 0; i < 6; i++) {
        // The box corner furthest along the plane normal: if even that is outside, the box is.
        float x = f->p[i][0] >= 0.0f ? mx.x : mn.x;
        float y = f->p[i][1] >= 0.0f ? mx.y : mn.y;
        float z = f->p[i][2] >= 0.0f ? mx.z : mn.z;
        if (f->p[i][0] * x + f->p[i][1] * y + f->p[i][2] * z + f->p[i][3] < 0.0f) {
            return false;
        }
    }
    return true;
}

float wl_aspect_for(bool widescreen) {
    return widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
}

// World planes carried into the model's space: a plane vector (n, d) is a row vector, and for x_world = M x_model + t the
// model-space plane is (M^T n, d + n.t). Exact for any affine matrix (the plane need not stay unit length).
bool wl_frustum_aabb_xform(const WlFrustum *f, const WlMat34 *world, WlVec3 mn, WlVec3 mx) {
    const float *m = world->m;
    for (int i = 0; i < 6; i++) {
        const float *p = f->p[i];
        float nx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2];
        float ny = m[1] * p[0] + m[5] * p[1] + m[9] * p[2];
        float nz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2];
        float d = p[3] + m[3] * p[0] + m[7] * p[1] + m[11] * p[2];
        float x = nx >= 0.0f ? mx.x : mn.x;
        float y = ny >= 0.0f ? mx.y : mn.y;
        float z = nz >= 0.0f ? mx.z : mn.z;
        if (nx * x + ny * y + nz * z + d < 0.0f) {
            return false;
        }
    }
    return true;
}
