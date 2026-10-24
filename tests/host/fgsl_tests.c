// Host tests for the FGSL compiler (SDK fglsl.h), the kernel's shader
// checker and machine (fgpu_shader.c) and the shader path of the
// rasterizer (fgpu_raster.c). Build and run: make test-host
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/fgpu_raster.h"
#include "fglsl.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static fglsl_program_t comp;
static fgpu_program_t prog;
static float uniforms[FGPU_SHADER_MAX_UNIFORMS][4];

static const char* trivial_vs =
    "attribute vec3 position;\n"
    "void main() { gl_Position = vec4(position, 1.0); }\n";

// Compiles and checks a program; prints the error if it fails.
static int build(const char* vs, const char* fs) {
    if (fglsl_compile(&comp, vs, fs) != 0) {
        printf("  compile error: %s\n", comp.error);
        return 0;
    }
    const char* why = NULL;
    if (!fgpu_program_check(comp.words, comp.size, &prog, &why)) {
        printf("  check error: %s\n", why);
        return 0;
    }
    return 1;
}

static void set_uniform(const char* name, const float* v, int n) {
    int i = fglsl_find_uniform(&comp, name);
    if (i < 0) { printf("  no uniform %s\n", name); failures++; return; }
    memcpy(uniforms[comp.uniforms[i].reg], v, (size_t)n * sizeof(float));
}

// Runs the fragment stage once with the given varyings.
static int run_fs(const float var[][4], float out[4]) {
    float special[2][4] = { { 10.5f, 20.5f, 0.5f, 1.0f }, { 1, 1, 1, 1 } };
    float o[1][4];
    fgpu_shader_io_t io = { &prog, var, o, (const float (*)[4])uniforms, (const float (*)[4])special, NULL };
    int kept = fgpu_shader_run_fragment(&io);
    memcpy(out, o[0], sizeof(o[0]));
    return kept;
}

// Compiles a fragment shader whose main() is `body` and returns its color.
static int eval(const char* decls, const char* body, float out[4]) {
    static char fs[4096];
    snprintf(fs, sizeof fs, "%s\nvoid main() {\n%s\n}\n", decls, body);
    if (!build(trivial_vs, fs)) return 0;
    float var[1][4] = { { 0 } };
    return run_fs(var, out) ? 1 : 2;
}

/* ------------------------------------------------------------ language */

static void test_math(void) {
    float o[4];
    CHECK(eval("", "gl_FragColor = vec4(1.0, 2.0, 3.0, 4.0) * 0.5 + vec4(0.25);", o) == 1, "arith compiles");
    CHECK(NEAR(o[0], 0.75f, 1e-6f) && NEAR(o[3], 2.25f, 1e-6f), "constant folding / broadcast");

    CHECK(eval("uniform float x;", "gl_FragColor = vec4(sin(x), cos(x), pow(x, 2.5), exp2(x));", o) == 1, "builtins compile");
    float x = 1.3f;
    set_uniform("x", &x, 1);
    float var[1][4] = { { 0 } };
    run_fs(var, o);
    CHECK(NEAR(o[0], sinf(x), 1e-5f) && NEAR(o[1], cosf(x), 1e-5f), "sin/cos");
    CHECK(NEAR(o[2], powf(x, 2.5f), 1e-4f) && NEAR(o[3], exp2f(x), 1e-4f), "pow/exp2");

    float args[] = { 0.1f, 2.0f, 7.25f, 123.0f, 1e-3f };
    for (unsigned i = 0; i < sizeof args / sizeof args[0]; i++) {
        eval("uniform float x;", "gl_FragColor = vec4(log2(x), sqrt(x), inversesqrt(x), atan(x, 1.0));", o);
        set_uniform("x", &args[i], 1);
        run_fs(var, o);
        CHECK(NEAR(o[0], log2f(args[i]), 1e-4f), "log2");
        CHECK(NEAR(o[1], sqrtf(args[i]), 1e-4f * sqrtf(args[i]) + 1e-6f), "sqrt");
        CHECK(NEAR(o[2], 1.0f / sqrtf(args[i]), 1e-3f / sqrtf(args[i])), "inversesqrt");
        CHECK(NEAR(o[3], atan2f(args[i], 1.0f), 1e-4f), "atan");
    }

    eval("uniform vec3 v;", "vec3 n = normalize(v); gl_FragColor = vec4(n, length(v));", o);
    float v[3] = { 3, 0, 4 };
    set_uniform("v", v, 3);
    run_fs(var, o);
    CHECK(NEAR(o[0], 0.6f, 1e-5f) && NEAR(o[2], 0.8f, 1e-5f) && NEAR(o[3], 5.0f, 1e-4f), "normalize/length");

    eval("", "vec3 a = vec3(1.0, 0.0, 0.0); vec3 b = vec3(0.0, 1.0, 0.0);\n"
             "gl_FragColor = vec4(cross(a, b), dot(a + b, b));", o);
    CHECK(NEAR(o[2], 1.0f, 1e-6f) && NEAR(o[3], 1.0f, 1e-6f), "cross/dot");

    eval("", "gl_FragColor = vec4(clamp(1.5, 0.0, 1.0), mix(2.0, 4.0, 0.25), step(0.5, 0.7), smoothstep(0.0, 1.0, 0.5));", o);
    CHECK(o[0] == 1.0f && NEAR(o[1], 2.5f, 1e-6f) && o[2] == 1.0f && NEAR(o[3], 0.5f, 1e-6f), "clamp/mix/step/smoothstep");

    eval("", "gl_FragColor = vec4(floor(-1.5), ceil(1.2), fract(2.75), mod(7.0, 3.0));", o);
    CHECK(o[0] == -2.0f && o[1] == 2.0f && NEAR(o[2], 0.75f, 1e-6f) && NEAR(o[3], 1.0f, 1e-6f), "floor/ceil/fract/mod");

    eval("", "vec3 r = reflect(vec3(1.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0)); gl_FragColor = vec4(r, abs(-3.0));", o);
    CHECK(o[0] == 1.0f && o[1] == 1.0f && o[3] == 3.0f, "reflect/abs");
}

static void test_swizzle_and_control(void) {
    float o[4];
    eval("", "vec4 v = vec4(1.0, 2.0, 3.0, 4.0); v.xz = v.zx; vec2 w = v.wy; gl_FragColor = vec4(w, v.xz);", o);
    CHECK(o[0] == 4 && o[1] == 2 && o[2] == 3 && o[3] == 1, "swizzle read/write");

    eval("uniform float k;", "float r = 0.0;\n if (k > 0.5) { r = 1.0; } else if (k > 0.25) r = 2.0; else r = 3.0;\n"
                             "gl_FragColor = vec4(r, k > 0.5 ? 10.0 : 20.0, !(k > 0.5) ? 1.0 : 0.0, 0.0);", o);
    float var[1][4] = { { 0 } };
    float ks[3] = { 0.9f, 0.3f, 0.1f }, want[3] = { 1, 2, 3 };
    for (int i = 0; i < 3; i++) {
        set_uniform("k", &ks[i], 1);
        run_fs(var, o);
        CHECK(o[0] == want[i], "if / else if / else");
        CHECK(o[1] == (i == 0 ? 10.0f : 20.0f) && o[2] == (i == 0 ? 0.0f : 1.0f), "ternary and !");
    }

    eval("", "float s = 0.0;\n for (int i = 0; i < 10; i++) { if (i == 7) break; if (i == 2) continue; s += float(i); }\n"
             "for (int j = 3; j > 0; j--) s += 100.0;\n gl_FragColor = vec4(s);", o);
    CHECK(o[0] == 0 + 1 + 3 + 4 + 5 + 6 + 300, "unrolled loops with break/continue");
    eval("", "float s = 0.0;\n for (int i = 0; i < 4; i++) if (i < 2) s += 1.0; else s += 10.0;\n"
             "for (int k = 0; k < 0; k++) if (k > 0) s = 99.0; else s = 98.0;\n gl_FragColor = vec4(s);", o);
    CHECK(o[0] == 22.0f, "unbraced if/else loop bodies, zero-trip loop");

    eval("float sq(float x) { return x * x; }\n"
         "vec3 tint(vec3 c, float k) { if (k > 1.0) { return c; } return c * k; }\n"
         "float twice(float x) { return sq(x) + sq(x); }",
         "gl_FragColor = vec4(tint(vec3(1.0, 2.0, 3.0), 0.5), twice(3.0));", o);
    CHECK(o[0] == 0.5f && o[2] == 1.5f && o[3] == 18.0f, "user functions (inlined, early return)");

    eval("uniform vec4 pal[3];\nconst float SCALE = 2.0;", "gl_FragColor = pal[2] * SCALE;", o);
    float pal[3][4] = { { 1, 1, 1, 1 }, { 2, 2, 2, 2 }, { 0.5f, 0.25f, 1, 0 } };
    set_uniform("pal", &pal[0][0], 12);
    run_fs(var, o);
    CHECK(o[0] == 1.0f && o[1] == 0.5f, "uniform arrays and global consts");

    CHECK(eval("", "if (gl_FragCoord.x > 5.0) discard; gl_FragColor = vec4(1.0);", o) == 2, "discard");
    CHECK(prog.h.flags & FGPU_SHADER_FLAG_DISCARD, "discard flag set");

    eval("", "gl_FragColor = vec4(gl_FragCoord.xy, gl_FrontFacing ? 1.0 : 0.0, 1.0);", o);
    CHECK(o[0] == 10.5f && o[1] == 20.5f && o[2] == 1.0f, "gl_FragCoord / gl_FrontFacing");

    // GLSL 1.30 style: in/out and a named fragment output.
    CHECK(build("#version 130\nin vec3 position; out vec2 v_uv;\n"
                "void main() { v_uv = position.xy * 0.5 + 0.5; gl_Position = vec4(position, 1.0); }",
                "#version 130\nprecision mediump float;\nin vec2 v_uv; out vec4 color;\n"
                "void main() { color = vec4(v_uv, 0.0, 1.0); }"), "in/out syntax");
    float uv[1][4] = { { 0.25f, 0.75f, 0, 0 } };
    run_fs(uv, o);
    CHECK(o[0] == 0.25f && o[1] == 0.75f, "varying read");
}

static void test_errors(void) {
    struct { const char* fs; const char* expect; } cases[] = {
        { "void main() { gl_FragColor = vec4(nothing); }", "unknown identifier 'nothing'" },
        { "void main() { gl_FragColor = vec3(1.0); }", "type mismatch" },
        { "varying vec3 v_missing; void main() { gl_FragColor = vec4(v_missing, 1.0); }", "does not write varying" },
        { "float f(float x) { return f(x); } void main() { gl_FragColor = vec4(f(1.0)); }", "recursion" },
        { "void main() { float s = 0.0; while (s < 1.0) s += 1.0; gl_FragColor = vec4(s); }", "only for loops" },
        { "void main() { gl_FragColor = vec4(1.0) }", "expected ';'" },
        { "uniform mat3 m; void main() { }", "only mat4" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = fglsl_compile(&comp, trivial_vs, cases[i].fs);
        CHECK(r != 0 && strstr(comp.error, cases[i].expect), cases[i].expect);
        if (r == 0 || !strstr(comp.error, cases[i].expect)) printf("  got: %s\n", comp.error);
    }
    CHECK(fglsl_compile(&comp, trivial_vs, "void main() {\n\n gl_FragColor = vec4(oops); }") != 0 &&
          strstr(comp.error, "fragment shader line 3"), "error names stage and line");
}

/* ----------------------------------------------------------- validator */

static void test_validator(void) {
    CHECK(build(trivial_vs, "void main() { gl_FragColor = vec4(1.0); }"), "baseline builds");
    uint32_t blob[FGLSL_MAX_BLOB_WORDS];
    memcpy(blob, comp.words, comp.size);
    fgpu_shader_header_t* h = (fgpu_shader_header_t*)blob;
    uint32_t* fs = blob + h->fs_offset;
    const char* why;

    CHECK(fgpu_program_check(blob, comp.size, &prog, &why), "copy is valid");
    uint32_t save[5];
    memcpy(save, fs, sizeof save);

    fs[0] = FGPU_SOP_JMP; fs[1] = 0;                    // backward jump (loop)
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "backward jump rejected");
    fs[1] = h->fs_count + 1;
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "jump past end rejected");
    memcpy(fs, save, sizeof save);

    fs[0] = FGPU_SOP_MOV;
    fs[1] = FGPU_DST(FGPU_FILE_TEMP, 200, 0xF);         // temp out of range
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "bad temp rejected");
    fs[1] = FGPU_DST(FGPU_FILE_UNIFORM, 0, 0xF);        // writing a uniform
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "write to uniform rejected");
    fs[1] = FGPU_DST(FGPU_FILE_OUTPUT, 0, 0xF);
    fs[2] = FGPU_SRC(FGPU_FILE_CONST, 250, 0);          // constant out of range
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "bad constant rejected");
    fs[2] = FGPU_SRC(FGPU_FILE_INPUT, 3, 0);            // varying the program does not have
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "bad varying rejected");
    fs[0] = 0xFF;
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "unknown opcode rejected");
    memcpy(fs, save, sizeof save);

    h->fs_count = 100000;
    CHECK(!fgpu_program_check(blob, comp.size, NULL, &why), "huge count rejected");
    CHECK(!fgpu_program_check(blob, comp.size - 4, NULL, &why), "truncated blob rejected");
}

/* ------------------------------------------------------------ pipeline */

static void mat_perspective(float m[16], float fovy, float aspect, float n, float f) {
    float t = 1.0f / tanf(fovy / 2);
    memset(m, 0, 16 * sizeof(float));
    m[0] = t / aspect; m[5] = t; m[10] = (f + n) / (n - f); m[11] = -1; m[14] = 2 * f * n / (n - f);
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

static void mat_rot_y(float m[16], float a) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = cosf(a); m[8] = sinf(a); m[2] = -sinf(a); m[10] = cosf(a); m[5] = 1; m[15] = 1;
}

static void test_pipeline(void) {
    enum { W = 160, H = 120 };
    static uint32_t c1[W * H], c2[W * H];
    static float d1[W * H], d2[W * H];
    fgpu_target_t t1 = { c1, W, d1, W, W, H }, t2 = { c2, W, d2, W, W, H };

    // A colored, rotated cube drawn by the built-in COLOR program and by an
    // equivalent shader must give identical pixels.
    float verts[36 * 10];
    int n = 0;
    static const float faces[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    for (int f = 0; f < 6; f++) {
        float N[3] = { faces[f][0], faces[f][1], faces[f][2] }, U[3], V[3];
        if (N[0] != 0) { U[0] = 0; U[1] = 0; U[2] = -N[0]; V[0] = 0; V[1] = 1; V[2] = 0; }
        else if (N[1] != 0) { U[0] = 1; U[1] = 0; U[2] = 0; V[0] = 0; V[1] = 0; V[2] = -N[1]; }
        else { U[0] = N[2]; U[1] = 0; U[2] = 0; V[0] = 0; V[1] = 1; V[2] = 0; }
        const float su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++, n++) {
            float* o = verts + n * 10;
            for (int i = 0; i < 3; i++) o[i] = N[i] + su[order[k]] * U[i] + sv[order[k]] * V[i];
            memcpy(o + 3, N, 12);
            o[6] = 0.2f + 0.13f * f; o[7] = 1.0f - 0.15f * f; o[8] = 0.5f; o[9] = 1;
        }
    }
    float proj[16], view[16], rot[16], tmp[16], mvp[16];
    mat_perspective(proj, 1.0f, (float)W / H, 0.1f, 50.0f);
    memset(view, 0, sizeof view);
    view[0] = view[5] = view[10] = view[15] = 1; view[14] = -4.5f;
    mat_rot_y(rot, 0.6f);
    mat_mul(view, rot, tmp);
    mat_mul(proj, tmp, mvp);

    fgpu_pipeline_t p;
    memset(&p, 0, sizeof p);
    p.vp_w = W; p.vp_h = H;
    p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_CULL_BACK;
    p.program = FGPU_PROGRAM_COLOR;
    memcpy(p.u.mvp, mvp, sizeof mvp);
    fgpu_raster_clear(&t1, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x000000, 1.0f);
    CHECK(fgpu_raster_draw(&t1, &p, verts, 36, NULL, 36) == 12, "fixed cube draws");

    CHECK(build("uniform mat4 fgl_ModelViewProjection;\n"
                "attribute vec3 position; attribute vec4 color; varying vec4 v_color;\n"
                "void main() { v_color = color; gl_Position = fgl_ModelViewProjection * vec4(position, 1.0); }",
                "varying vec4 v_color; void main() { gl_FragColor = v_color; }"), "color shader builds");
    memset(uniforms, 0, sizeof uniforms);
    set_uniform("fgl_ModelViewProjection", mvp, 16);
    p.shader = &prog;
    p.constants = (const float (*)[4])uniforms;
    fgpu_raster_clear(&t2, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x000000, 1.0f);
    CHECK(fgpu_raster_draw(&t2, &p, verts, 36, NULL, 36) == 12, "shader cube draws");
    int diff = 0, covered = 0;
    for (int i = 0; i < W * H; i++) {
        diff += c1[i] != c2[i];
        covered += c1[i] != 0xFF000000u;
    }
    CHECK(covered > 2000, "cube is visible");
    CHECK(diff == 0, "shader output matches the built-in program pixel for pixel");
    if (diff) printf("  %d pixels differ\n", diff);

    // Textured full-screen quad through a custom layout (position, uv).
    static uint32_t tex[8 * 8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) tex[y * 8 + x] = ((x ^ y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    float quad[6 * 5] = {
        -1, -1, 0, 0, 0,   1, -1, 0, 1, 0,   1, 1, 0, 1, 1,
        -1, -1, 0, 0, 0,   1,  1, 0, 1, 1,  -1, 1, 0, 0, 1 };
    fgpu_layout_t layout = { 5, 2, { FGPU_ATTRIB(0, 3), FGPU_ATTRIB(3, 2) } };
    CHECK(fgpu_layout_valid(&layout), "layout valid");
    CHECK(build("layout(location = 0) in vec3 pos; layout(location = 1) in vec2 uv; varying vec2 v_uv;\n"
                "void main() { v_uv = uv; gl_Position = vec4(pos, 1.0); }",
                "uniform sampler2D tex; varying vec2 v_uv;\n"
                "void main() { vec4 c = texture2D(tex, v_uv); if (v_uv.x > 0.75) discard; gl_FragColor = c; }"),
          "texture shader builds");
    memset(&p, 0, sizeof p);
    p.vp_w = W; p.vp_h = H;
    p.shader = &prog;
    p.constants = (const float (*)[4])uniforms;
    p.textures[0] = (fgpu_texture_t){ tex, 8, 8, 8, 0 };
    fgpu_raster_clear(&t1, FGPU_CLEAR_COLOR, 0x123456, 1.0f);
    CHECK(fgpu_raster_draw_layout(&t1, &p, &layout, quad, 6, NULL, 6) == 2, "quad draws");
    // Texel (0,7) is at screen top-left (v grows upward): (0 ^ 7) & 1 = 1 -> white.
    CHECK(c1[0] == 0xFFFFFFFFu, "nearest texel at top-left");
    CHECK(c1[(H - 1) * W] == 0xFF000000u, "nearest texel at bottom-left");
    CHECK(c1[W - 1] == 0xFF123456u, "discarded fragments keep the background");

    // Blending: half-transparent red over blue.
    CHECK(build(trivial_vs, "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 0.5); }"), "blend shader builds");
    p.state = FGPU_STATE_BLEND;
    fgpu_raster_clear(&t1, FGPU_CLEAR_COLOR, 0x0000FF, 1.0f);
    fgpu_raster_draw(&t1, &p, quad, 6, NULL, 6);    // default layout reads 10 floats: use layout instead
    fgpu_raster_clear(&t1, FGPU_CLEAR_COLOR, 0x0000FF, 1.0f);
    fgpu_layout_t pos_only = { 5, 1, { FGPU_ATTRIB(0, 3) } };
    fgpu_raster_draw_layout(&t1, &p, &pos_only, quad, 6, NULL, 6);
    uint32_t px = c1[(H / 2) * W + W / 2];
    CHECK(px == 0xFF800080u || px == 0xFF7F0080u || px == 0xFF80007Fu, "50% blend of red over blue");
}


/* Splitting a draw over cores must not change a single pixel. The fake
 * runner below runs the parts backwards on five "workers". */
static uint32_t fake_workers(void) { return 5; }
static void fake_run(fgpu_part_fn fn, void* ctx, uint32_t parts) {
    for (uint32_t i = parts; i-- > 0;) fn(ctx, i, (i * 7u) % 5u);
}

// Overlapping, half-transparent waves: 2 x 40 x 12 quads (1920 triangles,
// more than one chunk), so draw order and chunking both matter.
static int wave_mesh(float* v) {
    int n = 0;
    for (int layer = 0; layer < 2; layer++)
        for (int gy = 0; gy < 12; gy++)
            for (int gx = 0; gx < 40; gx++) {
                float x0 = -1.2f + gx * 0.06f, x1 = x0 + 0.06f;
                float y0 = -1.0f + gy * 0.17f + layer * 0.08f, y1 = y0 + 0.17f;
                float z0 = 0.3f * sinf(x0 * 3 + layer), z1 = 0.3f * sinf(x1 * 3 + layer);
                const float q[6][3] = { {x0,y0,z0}, {x1,y0,z1}, {x1,y1,z1}, {x0,y0,z0}, {x1,y1,z1}, {x0,y1,z0} };
                for (int k = 0; k < 6; k++, n++) {
                    float* o = v + n * 10;
                    o[0] = q[k][0]; o[1] = q[k][1]; o[2] = q[k][2];
                    o[3] = 0; o[4] = 0; o[5] = 1;
                    o[6] = layer ? 1.0f : 0.2f; o[7] = 0.1f * (gx % 10); o[8] = 0.08f * gy; o[9] = 0.6f;
                }
            }
    return n;
}

static void test_parallel(void) {
    enum { W = 200, H = 150 };
    static uint32_t c1[W * H], c2[W * H];
    static float d1[W * H], d2[W * H];
    static float verts[2 * 40 * 12 * 6 * 10];
    int n = wave_mesh(verts);
    fgpu_parallel_t par = { fake_workers, fake_run };

    float proj[16], view[16], rot[16], tmp[16], mvp[16];
    mat_perspective(proj, 1.0f, (float)W / H, 0.1f, 50.0f);
    memset(view, 0, sizeof view);
    view[0] = view[5] = view[10] = view[15] = 1; view[14] = -2.5f;
    mat_rot_y(rot, 0.4f);
    mat_mul(view, rot, tmp);
    mat_mul(proj, tmp, mvp);

    CHECK(build("uniform mat4 fgl_ModelViewProjection;\n"
                "attribute vec3 position; attribute vec4 color; varying vec4 v_color; varying vec3 v_pos;\n"
                "void main() { v_color = color; v_pos = position; gl_Position = fgl_ModelViewProjection * vec4(position, 1.0); }",
                "varying vec4 v_color; varying vec3 v_pos;\n"
                "void main() { if (fract(v_pos.x * 7.0) < 0.1) discard;\n"
                "  float s = 0.5 + 0.5 * sin(v_pos.y * 20.0);\n"
                "  gl_FragColor = vec4(v_color.rgb * s, v_color.a); }"), "parallel test shader builds");
    memset(uniforms, 0, sizeof uniforms);
    set_uniform("fgl_ModelViewProjection", mvp, 16);

    // Shader + blending + depth, built-in Phong, wireframe, and a viewport.
    for (int mode = 0; mode < 4; mode++) {
        fgpu_pipeline_t p;
        memset(&p, 0, sizeof p);
        p.vp_w = W; p.vp_h = H;
        p.state = FGPU_STATE_DEPTH_TEST | FGPU_STATE_DEPTH_WRITE | FGPU_STATE_BLEND;
        memcpy(p.u.mvp, mvp, sizeof mvp);
        p.u.model[0] = p.u.model[5] = p.u.model[10] = p.u.model[15] = 1;
        memcpy(p.u.normal, p.u.model, sizeof p.u.model);
        p.u.light_dir[2] = -1; p.u.light_color[0] = p.u.light_color[1] = p.u.light_color[2] = 1;
        p.u.camera_pos[2] = 2.5f; p.u.material[0] = 0.5f; p.u.material[1] = 16;
        if (mode == 0) { p.shader = &prog; p.constants = (const float (*)[4])uniforms; }
        else p.program = FGPU_PROGRAM_PHONG;
        if (mode == 2) p.state |= FGPU_STATE_WIREFRAME;
        if (mode == 3) { p.vp_x = 17; p.vp_y = -9; p.vp_w = 150; p.vp_h = 140; }

        uint32_t drawn[2];
        for (int run = 0; run < 2; run++) {
            fgpu_raster_set_parallel(run ? &par : NULL);
            fgpu_target_t t = run ? (fgpu_target_t){ c2, W, d2, W, W, H } : (fgpu_target_t){ c1, W, d1, W, W, H };
            fgpu_raster_clear(&t, FGPU_CLEAR_COLOR | FGPU_CLEAR_DEPTH, 0x203040, 1.0f);
            drawn[run] = fgpu_raster_draw(&t, &p, verts, (uint32_t)n, NULL, (uint32_t)n);
        }
        fgpu_raster_set_parallel(NULL);
        int diff = 0, covered = 0;
        for (int i = 0; i < W * H; i++) {
            diff += c1[i] != c2[i] || d1[i] != d2[i];
            covered += c1[i] != 0xFF203040u;
        }
        CHECK(drawn[0] == drawn[1] && drawn[0] > 1000, "parallel draw counts match");
        CHECK(covered > 3000, "parallel scene is visible");
        CHECK(diff == 0, "parallel rendering matches one core pixel for pixel");
        if (diff) printf("  mode %d: %d pixels differ\n", mode, diff);
    }
}

int main(void) {
    test_math();
    test_swizzle_and_control();
    test_errors();
    test_validator();
    test_pipeline();
    test_parallel();
    if (failures) {
        printf("fgsl tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("fgsl tests passed\n");
    return 0;
}
