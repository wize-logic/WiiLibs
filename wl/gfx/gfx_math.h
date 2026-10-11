// SPDX-License-Identifier: MIT
// Vectors, 3x4 and 4x4 matrices, cameras and frustum culling for a GX renderer.
//
// Layouts follow GX so matrices go to the GPU unchanged: WlMat34 is a row-major 3x4 position matrix
// (libogc's Mtx; the fourth column is the translation), WlMat44 a row-major 4x4 projection (Mtx44).
// Convention: right-handed, +Y up, the camera looks down -Z (what guLookAt and guPerspective do).
// This is plain C so host tests can run it; the paired-single routines (gfx_ps.h) cover the per-bone
// and per-vertex work of skinning. Floats are fine here: this is presentation code, not simulation
// that has to be bit-identical across machines.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float x, y, z;
} WlVec3;

typedef struct {
    float m[12]; // rows 0..2, columns 0..3
} WlMat34;

typedef struct {
    float m[16];
} WlMat44;

WlVec3 wl_vec3(float x, float y, float z);
WlVec3 wl_vec3_add(WlVec3 a, WlVec3 b);
WlVec3 wl_vec3_sub(WlVec3 a, WlVec3 b);
WlVec3 wl_vec3_scale(WlVec3 a, float s);
float wl_vec3_dot(WlVec3 a, WlVec3 b);
WlVec3 wl_vec3_cross(WlVec3 a, WlVec3 b);
float wl_vec3_length(WlVec3 a);
// Unit vector; the zero vector stays zero.
WlVec3 wl_vec3_normalize(WlVec3 a);

void wl_mat34_identity(WlMat34 *out);
// out = a * b (b is applied first). `out` may alias a or b.
void wl_mat34_mul(WlMat34 *out, const WlMat34 *a, const WlMat34 *b);
void wl_mat34_translate(WlMat34 *out, float x, float y, float z);
void wl_mat34_scale(WlMat34 *out, float x, float y, float z);
void wl_mat34_rotate_x(WlMat34 *out, float radians);
void wl_mat34_rotate_y(WlMat34 *out, float radians);
void wl_mat34_rotate_z(WlMat34 *out, float radians);
// Rotation about a (not necessarily unit) axis.
void wl_mat34_rotate_axis(WlMat34 *out, WlVec3 axis, float radians);
// T(t) * R(q) * S(s): `q` is x y z w and is normalised here (a zero quaternion is the identity), `s` scales
// the local axes before the rotation (NULL = 1). The usual bone matrix of a skeleton.
void wl_mat34_from_trs(WlMat34 *out, const float t[3], const float q[4], const float s[3]);
// Inverse of an affine matrix; false (and `out` untouched) when singular. `out` may alias `in`.
bool wl_mat34_inverse(WlMat34 *out, const WlMat34 *in);
WlVec3 wl_mat34_transform_point(const WlMat34 *m, WlVec3 p);
// Rotation and scale only (no translation).
WlVec3 wl_mat34_transform_dir(const WlMat34 *m, WlVec3 d);
// World-to-view matrix of a camera at `eye` looking at `target`.
void wl_mat34_look_at(WlMat34 *out, WlVec3 eye, WlVec3 target, WlVec3 up);

// The matrices guPerspective and guOrtho build (clip z in [-1, 0] for near..far, as GX wants).
void wl_mat44_perspective(WlMat44 *out, float fovy_degrees, float aspect, float znear, float zfar);
void wl_mat44_ortho(WlMat44 *out, float top, float bottom, float left, float right, float znear, float zfar);

// ---------------------------------------------------------------------------------------------
// Cameras and culling
// ---------------------------------------------------------------------------------------------

typedef struct {
    WlVec3 eye, target, up;
    float fovy_degrees; // perspective
    float aspect;       // width / height: 4/3, or 16/9 on a widescreen console
    float znear, zfar;
    bool ortho;         // orthographic: `ortho_height` world units tall instead of the field of view
    float ortho_height;
} WlCamera;

// A perspective camera at (0, 0, 10) looking at the origin; fov 45, near 0.5, far 500.
void wl_camera_default(WlCamera *cam, float aspect);
void wl_camera_view(const WlCamera *cam, WlMat34 *out);
// Places the camera on a sphere around `target`: yaw about +Y (0 = on +Z looking toward -Z), pitch
// above the horizon (positive = looking down), distance.
void wl_camera_orbit(WlCamera *cam, WlVec3 target, float yaw_radians, float pitch_radians, float distance);
// Projection matrix the renderer uploads for this camera.
void wl_camera_projection(const WlCamera *cam, WlMat44 *out);
// World point to viewport pixels (x right, y down, origin top left). `out` = {x, y, view distance}.
// False when the point is behind the camera (out is still filled where defined).
bool wl_camera_project(const WlCamera *cam, WlVec3 world, float vp_width, float vp_height, float out[3]);

typedef struct {
    float p[6][4]; // world-space planes a, b, c, d: a*x + b*y + c*z + d >= 0 inside. left right bottom top near far
} WlFrustum;
void wl_camera_frustum(const WlCamera *cam, WlFrustum *out);
// True unless the sphere / box is entirely outside one plane (conservative: may keep invisible ones).
bool wl_frustum_sphere(const WlFrustum *f, WlVec3 center, float radius);
bool wl_frustum_aabb(const WlFrustum *f, WlVec3 min, WlVec3 max);
// The same for a box given in a model's space and the model-to-world matrix (any affine matrix, scale and shear
// included): the planes are carried into model space, so the box is never transformed.
bool wl_frustum_aabb_xform(const WlFrustum *f, const WlMat34 *world, WlVec3 min, WlVec3 max);

// Projection aspect for the console's video setting: 16/9 when widescreen, else 4/3.
float wl_aspect_for(bool widescreen);
