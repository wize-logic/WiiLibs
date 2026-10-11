// SPDX-License-Identifier: MIT
// Vector, matrix and camera maths: multiplication order, inverses, rotations, look-at, the GX
// projection conventions, camera projection, frustum culling and orbit placement.
#include "wl/gfx/gfx_math.h"

#include <math.h>
#include <string.h>

#include "greatest.h"

#define NEAR_EQ(a, b, eps) ASSERT_IN_RANGE((double)(b), (double)(a), (double)(eps))

#define PI_F 3.14159265f
#define expect_vec(v, X, Y, Z, EPS)                                                                  \
    do {                                                                                             \
        WlVec3 v_ = (v);                                                                             \
        ASSERT_IN_RANGE((double)(X), (double)v_.x, (double)(EPS));                                   \
        ASSERT_IN_RANGE((double)(Y), (double)v_.y, (double)(EPS));                                   \
        ASSERT_IN_RANGE((double)(Z), (double)v_.z, (double)(EPS));                                   \
    } while (0)

// ---- maths ------------------------------------------------------------------------------------

TEST mat_mul_order_and_inverse(void) {
    WlMat34 t;
    WlMat34 s;
    WlMat34 m;
    WlMat34 inv;
    WlMat34 id;
    wl_mat34_translate(&t, 1, 2, 3);
    wl_mat34_scale(&s, 2, 2, 2);
    wl_mat34_mul(&m, &t, &s); // scale first, then translate
    expect_vec(wl_mat34_transform_point(&m, wl_vec3(1, 1, 1)), 3, 4, 5, 1e-5f);
    wl_mat34_mul(&m, &s, &t); // translate first, then scale
    expect_vec(wl_mat34_transform_point(&m, wl_vec3(1, 1, 1)), 4, 6, 8, 1e-5f);
    expect_vec(wl_mat34_transform_dir(&m, wl_vec3(1, 0, 0)), 2, 0, 0, 1e-5f);

    WlMat34 r;
    wl_mat34_rotate_axis(&r, wl_vec3(0.3f, 1.0f, -0.2f), 0.7f);
    wl_mat34_mul(&m, &t, &r);
    WlMat34 m2;
    wl_mat34_mul(&m2, &m, &s);
    ASSERT(wl_mat34_inverse(&inv, &m2));
    wl_mat34_mul(&id, &inv, &m2);
    for (int i = 0; i < 12; i++) {
        NEAR_EQ(i == 0 || i == 5 || i == 10 ? 1.0f : 0.0f, id.m[i], 1e-5);
    }
    WlMat34 sing;
    wl_mat34_scale(&sing, 1, 0, 1);
    WlMat34 before = sing;
    ASSERT(!wl_mat34_inverse(&inv, &sing));
    ASSERT_EQ(0, memcmp(&before, &sing, sizeof sing));
    // In-place inverse.
    WlMat34 c = m2;
    ASSERT(wl_mat34_inverse(&c, &c));
    wl_mat34_mul(&id, &c, &m2);
    NEAR_EQ(1.0f, id.m[0], 1e-5);
    NEAR_EQ(0.0f, id.m[3], 1e-5);
    PASS();
}

TEST rotations_follow_the_right_hand_rule(void) {
    WlMat34 r;
    wl_mat34_rotate_y(&r, PI_F / 2);
    expect_vec(wl_mat34_transform_point(&r, wl_vec3(1, 0, 0)), 0, 0, -1, 1e-6f); // +x turns toward -z
    wl_mat34_rotate_x(&r, PI_F / 2);
    expect_vec(wl_mat34_transform_point(&r, wl_vec3(0, 1, 0)), 0, 0, 1, 1e-6f);
    wl_mat34_rotate_z(&r, PI_F / 2);
    expect_vec(wl_mat34_transform_point(&r, wl_vec3(1, 0, 0)), 0, 1, 0, 1e-6f);
    WlMat34 a;
    wl_mat34_rotate_axis(&a, wl_vec3(0, 1, 0), PI_F / 2);
    expect_vec(wl_mat34_transform_point(&a, wl_vec3(1, 0, 0)), 0, 0, -1, 1e-6f);
    PASS();
}

TEST look_at_puts_the_target_ahead(void) {
    WlMat34 v;
    wl_mat34_look_at(&v, wl_vec3(3, 4, 5), wl_vec3(3, 4, -5), wl_vec3(0, 1, 0));
    expect_vec(wl_mat34_transform_point(&v, wl_vec3(3, 4, 5)), 0, 0, 0, 1e-6f);
    expect_vec(wl_mat34_transform_point(&v, wl_vec3(3, 4, -5)), 0, 0, -10, 1e-5f);
    expect_vec(wl_mat34_transform_point(&v, wl_vec3(4, 4, 5)), 1, 0, 0, 1e-6f);  // world +x is camera right
    expect_vec(wl_mat34_transform_point(&v, wl_vec3(3, 5, 5)), 0, 1, 0, 1e-6f);  // world +y is camera up
    // Looking straight down: still a valid basis.
    wl_mat34_look_at(&v, wl_vec3(0, 10, 0), wl_vec3(0, 0, 0), wl_vec3(0, 1, 0));
    expect_vec(wl_mat34_transform_point(&v, wl_vec3(0, 0, 0)), 0, 0, -10, 1e-5f);
    PASS();
}

TEST projection_matrices_match_gx_conventions(void) {
    WlMat44 p;
    wl_mat44_perspective(&p, 90.0f, 2.0f, 1.0f, 11.0f);
    // clip = P * (x, y, -dist, 1): near maps to z/w = -1, far to 0 (GX), x scaled by cot/aspect.
    float cn[4] = {p.m[3] + p.m[2] * 0, p.m[7], p.m[11] + p.m[10] * -1.0f, p.m[15] + p.m[14] * -1.0f};
    NEAR_EQ(-1.0, cn[2] / cn[3], 1e-6);
    float cf[4] = {0, 0, p.m[11] + p.m[10] * -11.0f, p.m[14] * -11.0f + p.m[15]};
    NEAR_EQ(0.0, cf[2] / cf[3], 1e-5);
    NEAR_EQ(0.5, p.m[0], 1e-6);
    NEAR_EQ(1.0, p.m[5], 1e-6);
    NEAR_EQ(-1.0, p.m[14], 1e-6);
    WlMat44 o;
    wl_mat44_ortho(&o, 1.0f, -1.0f, -2.0f, 2.0f, 0.0f, 10.0f);
    NEAR_EQ(0.5, o.m[0], 1e-6);
    NEAR_EQ(1.0, o.m[5], 1e-6);
    NEAR_EQ(-0.1, o.m[10], 1e-6);
    NEAR_EQ(-1.0, o.m[11], 1e-6); // z = 0 (near) -> -1
    PASS();
}

TEST camera_projects_and_culls(void) {
    WlCamera c;
    wl_camera_default(&c, 4.0f / 3.0f);
    c.eye = wl_vec3(0, 0, 10);
    c.target = wl_vec3(0, 0, 0);
    float out[3];
    ASSERT(wl_camera_project(&c, wl_vec3(0, 0, 0), 640, 480, out));
    NEAR_EQ(320.0, out[0], 1e-3);
    NEAR_EQ(240.0, out[1], 1e-3);
    NEAR_EQ(10.0, out[2], 1e-4);
    // A point on the top edge of the view at distance 10: y = 10 * tan(fov/2).
    float top = 10.0f * tanf(c.fovy_degrees * PI_F / 360.0f);
    ASSERT(wl_camera_project(&c, wl_vec3(0, top, 0), 640, 480, out));
    NEAR_EQ(0.0, out[1], 1e-2);
    ASSERT(wl_camera_project(&c, wl_vec3(top * 4.0f / 3.0f, 0, 0), 640, 480, out));
    NEAR_EQ(640.0, out[0], 1e-2);
    ASSERT(!wl_camera_project(&c, wl_vec3(0, 0, 20), 640, 480, out)); // behind

    WlFrustum f;
    wl_camera_frustum(&c, &f);
    ASSERT(wl_frustum_sphere(&f, wl_vec3(0, 0, 0), 1));
    ASSERT(!wl_frustum_sphere(&f, wl_vec3(0, 0, 20), 1));          // behind
    ASSERT(!wl_frustum_sphere(&f, wl_vec3(0, 0, -600), 1));        // beyond far
    ASSERT(!wl_frustum_sphere(&f, wl_vec3(0, 0, 9.9f), 0.1f));     // before near (eye at 10, near 0.5)
    ASSERT(!wl_frustum_sphere(&f, wl_vec3(top * 4.0f / 3.0f * 2.0f, 0, 0), 1)); // far to the right
    ASSERT(wl_frustum_sphere(&f, wl_vec3(top * 4.0f / 3.0f * 1.05f, 0, 0), 1));  // straddles the edge
    ASSERT(wl_frustum_aabb(&f, wl_vec3(-1, -1, -1), wl_vec3(1, 1, 1)));
    ASSERT(!wl_frustum_aabb(&f, wl_vec3(-1, -1, 11), wl_vec3(1, 1, 13)));
    ASSERT(!wl_frustum_aabb(&f, wl_vec3(-1, top * 1.5f, -1), wl_vec3(1, top * 2.0f, 1)));
    // A box larger than the view that contains the camera frustum fully is visible.
    ASSERT(wl_frustum_aabb(&f, wl_vec3(-1000, -1000, -1000), wl_vec3(1000, 1000, 1000)));

    // Orthographic camera: 10 units tall.
    c.ortho = true;
    c.ortho_height = 10.0f;
    ASSERT(wl_camera_project(&c, wl_vec3(0, 5, 0), 640, 480, out));
    NEAR_EQ(0.0, out[1], 1e-3);
    wl_camera_frustum(&c, &f);
    ASSERT(wl_frustum_sphere(&f, wl_vec3(6, 0, 0), 0.5f));  // half width = 5 * 4/3 = 6.67
    ASSERT(!wl_frustum_sphere(&f, wl_vec3(8, 0, 0), 0.5f));
    PASS();
}

TEST orbit_places_the_eye(void) {
    WlCamera c;
    wl_camera_default(&c, 1.0f);
    wl_camera_orbit(&c, wl_vec3(1, 2, 3), 0.0f, 0.0f, 5.0f);
    expect_vec(c.eye, 1, 2, 8, 1e-5f);
    wl_camera_orbit(&c, wl_vec3(0, 0, 0), PI_F / 2, 0.0f, 4.0f);
    expect_vec(c.eye, 4, 0, 0, 1e-5f);
    wl_camera_orbit(&c, wl_vec3(0, 0, 0), 0.0f, PI_F / 2, 4.0f);
    expect_vec(c.eye, 0, 4, 0, 1e-5f);
    NEAR_EQ(16.0 / 9.0, wl_aspect_for(true), 1e-6);
    NEAR_EQ(4.0 / 3.0, wl_aspect_for(false), 1e-6);
    PASS();
}

// ---- texture codecs ---------------------------------------------------------------------------

// Reference: the box is outside when all eight corners lie behind one plane.
static bool box_visible_by_corners(const WlFrustum *f, const WlMat34 *w, WlVec3 mn, WlVec3 mx) {
    WlVec3 c[8];
    for (int i = 0; i < 8; i++) {
        c[i] = wl_mat34_transform_point(w, wl_vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z));
    }
    for (int p = 0; p < 6; p++) {
        bool any_in = false;
        for (int i = 0; i < 8; i++) {
            if (f->p[p][0] * c[i].x + f->p[p][1] * c[i].y + f->p[p][2] * c[i].z + f->p[p][3] >= 0.0f) {
                any_in = true;
            }
        }
        if (!any_in) {
            return false;
        }
    }
    return true;
}

TEST frustum_box_in_model_space_equals_the_corner_test(void) {
    WlCamera cam;
    wl_camera_default(&cam, 4.0f / 3.0f);
    WlFrustum f;
    wl_camera_frustum(&cam, &f);
    uint32_t seed = 12345;
    uint32_t in = 0, out = 0;
    for (int n = 0; n < 4000; n++) {
        float v[9];
        for (int k = 0; k < 9; k++) {
            seed = seed * 1664525u + 1013904223u;
            v[k] = ((float)(seed >> 8) / 16777216.0f) * 2.0f - 1.0f;
        }
        WlMat34 t, r, s, m, tmp;
        wl_mat34_translate(&t, v[0] * 14.0f, v[1] * 9.0f, v[2] * 14.0f - 4.0f);
        wl_mat34_rotate_axis(&r, wl_vec3(v[3], v[4], v[5] + 1.5f), v[6] * 3.0f);
        wl_mat34_scale(&s, 0.4f + (v[7] + 1.0f), 0.4f + (v[8] + 1.0f) * 0.5f, 0.5f + (v[7] + 1.0f) * 0.7f);
        wl_mat34_mul(&tmp, &r, &s);
        wl_mat34_mul(&m, &t, &tmp);
        WlVec3 mn = wl_vec3(-1.0f, -0.5f, -0.25f), mx = wl_vec3(2.0f, 1.0f, 0.75f);
        bool a = wl_frustum_aabb_xform(&f, &m, mn, mx);
        bool b = box_visible_by_corners(&f, &m, mn, mx);
        ASSERT_EQ_FMT(b, a, "%d");
        a ? in++ : out++;
    }
    ASSERT(in > 200 && out > 200); // both outcomes are exercised
    PASS();
}

SUITE(gfx_math_suite) {
    RUN_TEST(mat_mul_order_and_inverse);
    RUN_TEST(rotations_follow_the_right_hand_rule);
    RUN_TEST(look_at_puts_the_target_ahead);
    RUN_TEST(projection_matrices_match_gx_conventions);
    RUN_TEST(camera_projects_and_culls);
    RUN_TEST(orbit_places_the_eye);
    RUN_TEST(frustum_box_in_model_space_equals_the_corner_test);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gfx_math_suite);
    GREATEST_MAIN_END();
}
