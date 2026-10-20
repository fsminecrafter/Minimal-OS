// Host tests for the FGPU CPU rasterizer. Build: make test-host
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static void mat_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(out, r, sizeof(r));
}

static void mat_perspective(float m[16], float fovy, float aspect, float n, float f) {
    float t = 1.0f / tanf(fovy / 2);
    memset(m, 0, 16 * sizeof(float));
    m[0] = t / aspect;
    m[5] = t;
    m[10] = (f + n) / (n - f);
    m[11] = -1;
    m[14] = 2 * f * n / (n - f);
}

static void mat_rotate(float m[16], float angle, float x, float y, float z) {
    float c = cosf(angle), s = sinf(angle), l = sqrtf(x * x + y * y + z * z);
    x /= l; y /= l; z /= l;
    mat_identity(m);
    m[0] = x * x * (1 - c) + c;     m[4] = x * y * (1 - c) - z * s; m[8] = x * z * (1 - c) + y * s;
    m[1] = y * x * (1 - c) + z * s; m[5] = y * y * (1 - c) + c;     m[9] = y * z * (1 - c) - x * s;
    m[2] = x * z * (1 - c) - y * s; m[6] = y * z * (1 - c) + x * s; m[10] = z * z * (1 - c) + c;
}

static int cube(float* v) {
    static const float faces[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float colors[6][3] = { {1,.3f,.3f}, {.3f,1,.3f}, {.3f,.3f,1}, {1,1,.3f}, {.3f,1,1}, {1,.3f,1} };
    int n = 0;
    for (int f = 0; f < 6; f++) {
        float N[3] = { faces[f][0], faces[f][1], faces[f][2] };
        // Two axes spanning the face, chosen so corners go counter-clockwise
        // seen from outside.
        float U[3], V[3];
        if (N[0] != 0) { U[0] = 0; U[1] = 0; U[2] = -N[0]; V[0] = 0; V[1] = 1; V[2] = 0; }
        else if (N[1] != 0) { U[0] = 1; U[1] = 0; U[2] = 0; V[0] = 0; V[1] = 0; V[2] = -N[1]; }
        else { U[0] = N[2]; U[1] = 0; U[2] = 0; V[0] = 0; V[1] = 1; V[2] = 0; }
        float c[4][3];
        const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 3; i++) c[k][i] = N[i] + su[k] * U[i] + sv[k] * V[i];
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) {
            float* o = v + n * 10;
            memcpy(o, c[order[k]], 12);
            memcpy(o + 3, N, 12);
            o[6] = colors[f][0]; o[7] = colors[f][1]; o[8] = colors[f][2]; o[9] = 1;
            n++;
        }
    }
    return n;
}

static void write_ppm(const char* path, const uint32_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8),
                                 (unsigned char)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    const int W = 320, H = 240;
    uint32_t* color = calloc((size_t)W * H, 4);
    float* depth = calloc((size_t)W * H, 4);
    fgpu_target_t t = { color, W, depth, W, W, H };

    fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x102030, 1.0f);
    CHECK(color[0] == 0xFF102030 && depth[W * H - 1] == 1.0f, "clear (opaque)");

    fgpu_pipeline_t p;
    memset(&p, 0, sizeof(p));
    p.vp_w = W; p.vp_h = H;
    p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_CULL_BACK;
    p.program = FGPU_PROGRAM_PHONG;

    float proj[16], view[16], model[16], rx[16], ry[16], tmp[16];
    mat_perspective(proj, 1.0f, (float)W / H, 0.1f, 100.0f);
    mat_identity(view);
    view[14] = -5.0f;
    mat_rotate(rx, 0.5f, 1, 0, 0);
    mat_rotate(ry, 0.7f, 0, 1, 0);
    mat_mul(rx, ry, model);
    mat_mul(view, model, tmp);
    mat_mul(proj, tmp, p.u.mvp);
    memcpy(p.u.model, model, sizeof(model));
    memcpy(p.u.normal, model, sizeof(model));
    p.u.light_dir[0] = -0.4f; p.u.light_dir[1] = -0.6f; p.u.light_dir[2] = -1.0f;
    p.u.light_color[0] = p.u.light_color[1] = p.u.light_color[2] = 1.0f;
    p.u.ambient[0] = p.u.ambient[1] = p.u.ambient[2] = 0.15f;
    p.u.camera_pos[2] = 5.0f;
    p.u.material[0] = 0.5f; p.u.material[1] = 32;

    float verts[36 * 10];
    int n = cube(verts);
    uint32_t drawn = fgpu_raster_draw(&t, &p, verts, (uint32_t)n, NULL, (uint32_t)n);
    CHECK(drawn == 12, "12 triangles submitted");

    uint32_t center = color[(H / 2) * W + W / 2];
    CHECK(center != 0xFF102030 && (center >> 24) == 0xFF, "cube covers the center, opaque");
    CHECK(color[0] == 0xFF102030, "corner stays background");
    CHECK(depth[(H / 2) * W + W / 2] < 1.0f, "depth written");
    int covered = 0;
    for (int i = 0; i < W * H; i++) covered += color[i] != 0xFF102030;
    CHECK(covered > 5000 && covered < 40000, "plausible coverage");
    if (argc > 1) write_ppm(argv[1], color, W, H);

    // Depth: a near red triangle drawn after a far green one must win,
    // and the far one drawn afterwards must not overwrite it.
    fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0, 1.0f);
    memset(&p.u, 0, sizeof(p.u));
    mat_identity(p.u.mvp); mat_identity(p.u.model); mat_identity(p.u.normal);
    p.program = FGPU_PROGRAM_COLOR;
    p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE;
    float tri_near[30] = { -1,-1,-0.5f, 0,0,1, 1,0,0,1,  1,-1,-0.5f, 0,0,1, 1,0,0,1,  0,1,-0.5f, 0,0,1, 1,0,0,1 };
    float tri_far[30]  = { -1,-1, 0.5f, 0,0,1, 0,1,0,1,  1,-1, 0.5f, 0,0,1, 0,1,0,1,  0,1, 0.5f, 0,0,1, 0,1,0,1 };
    fgpu_raster_draw(&t, &p, tri_near, 3, NULL, 3);
    fgpu_raster_draw(&t, &p, tri_far, 3, NULL, 3);
    CHECK(color[(H / 2) * W + W / 2] == 0xFFFF0000, "near triangle wins depth test");

    // Bad index must be skipped, not read out of bounds.
    uint32_t bad_idx[3] = { 0, 1, 7 };
    CHECK(fgpu_raster_draw(&t, &p, tri_near, 3, bad_idx, 3) == 0, "out of range index skipped");

    // Near-plane clipping: a triangle crossing the camera must not crash
    // and must still draw its visible part.
    fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0, 1.0f);
    mat_mul(proj, view, p.u.mvp);
    float crossing[30] = { -1,-1,4.9f, 0,0,1, 1,1,1,1,  1,-1,4.9f, 0,0,1, 1,1,1,1,  0,0,-20, 0,0,1, 1,1,1,1 };
    p.state = 0;
    fgpu_raster_draw(&t, &p, crossing, 3, NULL, 3);
    covered = 0;
    for (int i = 0; i < W * H; i++) covered += color[i] != 0xFF000000;
    CHECK(covered > 0, "clipped triangle still draws");

    free(color);
    free(depth);
    if (failures) return 1;
    printf("fgpu raster tests passed\n");
    return 0;
}
