# FarcadeGL (`farcadegl.h`)

FarcadeGL is the 3D library of Minimal-OS. It sits on the kernel's FGPU
interface (`SYS_GPU`, described in `fgpu_abi.h`) and borrows from both
OpenGL and Vulkan:

- **Like Vulkan**, drawing is recorded into a command buffer and sent to
  the GPU in one call (`fglSubmit` / `fglPresent`). GPU memory is explicit:
  you create and destroy buffers, textures and shaders.
- **Like OpenGL**, state is a handful of simple calls (`fglEnable`,
  `fglUseShader`, `fglSetModel` ...), matrices are column-major, and
  shaders are written in a GLSL-style language, **FGSL** ([fgsl.md](fgsl.md)).

It is header only: include `farcadegl.h` and build with `./build.sh`.

Contents:

1. [How it runs](#how-it-runs)
2. [A first program](#a-first-program)
3. [Contexts](#contexts)
4. [Buffers and vertex layouts](#buffers-and-vertex-layouts)
5. [State and drawing](#state-and-drawing)
6. [Built-in programs](#built-in-programs)
7. [Shaders](#shaders)
8. [Textures](#textures)
9. [Blending](#blending)
10. [Math](#math)
11. [Errors and limits](#errors-and-limits)
12. [Performance](#performance)
13. [Function reference](#function-reference)
14. [The FGPU interface underneath](#the-fgpu-interface-underneath)

Examples: `programs/fglcube` (shipped as `0:/programs/fglcube.run`; run
`fglcube --shader` for the shader version) and MOSdesk's *Cube* app
(press **S** for the shader).

## How it runs

The kernel executes command buffers through its FGPU command processor.
Today its backend runs on the CPU: vertex stage, clipping, perspective-
correct rasterization, depth test, fragment stage, blending. Shaders are
compiled in your program to FGPU bytecode, checked by the kernel when you
create them, and run per vertex and per pixel by the kernel's shader
machine. Surfaces and buffers are placed in the graphics card's video
memory when the GPU driver can hand some out (`gpu` in the terminal shows
the VRAM heap).

Programs do not depend on the backend: when a GPU driver can execute FGPU
work on the card itself (see `docs/gpu.md` in Minimal-OS for the plan for
NVIDIA cards), the same programs and the same shader bytecode run there.

## A first program

```c
#include "farcadegl.h"
#include "window.h"

static const char* vs =
    "uniform mat4 fgl_ModelViewProjection;\n"
    "attribute vec3 position;\n"
    "attribute vec4 color;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    v_color = color;\n"
    "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
    "}\n";

static const char* fs =
    "uniform float time;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    float pulse = 0.75 + 0.25 * sin(time * 4.0);\n"
    "    gl_FragColor = vec4(v_color.rgb * pulse, 1.0);\n"
    "}\n";

int main(void) {
    uint32_t win = mos_window_create(0, 100, 100, 640, 480, "Hello 3D");
    FGLContext* gl = fglCreateContext(&(FGLContextDesc){
        .width = 640, .height = 480, .window_id = win, .depth = 1 });

    FGLVertex cube[36];
    fglMakeCube(cube, 1.0f, NULL);
    FGLBuffer vb = fglCreateBuffer(gl, sizeof cube);
    fglBufferData(gl, vb, 0, cube, sizeof cube);

    FGLShader* sh = fglCreateShader(gl, vs, fs);
    if (!sh) { /* fglShaderLog(gl) says why */ return 1; }
    int u_time = fglUniformLocation(sh, "time");

    for (int frame = 0; frame < 600; frame++) {
        fglClearColor(gl, 0.05f, 0.07f, 0.12f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseShader(gl, sh);
        fglUniform1f(sh, u_time, frame / 60.0f);
        fglSetProjection(gl, fglPerspective(0.9f, 640.0f / 480.0f, 0.1f, 50));
        fglSetView(gl, fglLookAt(fglVec3(0, 1, 4), fglVec3(0, 0, 0), fglVec3(0, 1, 0)));
        fglSetModel(gl, fglRotate(frame * 0.02f, fglVec3(0, 1, 0)));
        fglDraw(gl, vb, 0, 36);
        fglPresent(gl);
    }
    fglDestroyShader(gl, sh);
    fglDestroyContext(gl);
    mos_window_close(win);
    return 0;
}
```

Without the shader lines, `fglUseProgram(gl, FGL_PROGRAM_PHONG)` draws the
same cube with the built-in per-pixel lighting.

## Contexts

```c
typedef struct {
    uint32_t width, height;     // render size
    uint32_t window_id;         // present into this window; 0 = the screen
    int32_t x, y;               // screen position when window_id == 0
    int depth;                  // create a depth buffer
    int page_flip;              // screen only: flip between two VRAM buffers
} FGLContextDesc;

FGLContext* fglCreateContext(const FGLContextDesc* desc);
void fglDestroyContext(FGLContext* gl);
```

A context owns the color surface(s) you draw into, an optional depth
buffer, and the command buffer being recorded.

- **In a window** (MOSdesk): create the window with `mos_window_create` and
  pass its id. `fglPresent` copies each frame into the window.
- **Full screen** (terminal): take the screen with `mos_graphics_acquire()`
  first. With `page_flip` set and a GPU that can flip (the QEMU/Bochs
  adapter can), frames are shown by switching the display to the buffer
  just drawn instead of copying it.

Returns `NULL` when the surfaces cannot be created. `fglGetDeviceInfo`
fills a `syscall_gpu_info_t` (GPU name, driver, VRAM, `backend`, `caps`;
`caps & FGPU_CAP_SHADERS` says shaders are available).

## Buffers and vertex layouts

```c
FGLBuffer fglCreateBuffer(FGLContext* gl, uint32_t bytes);
int fglBufferData(FGLContext* gl, FGLBuffer b, uint32_t offset, const void* data, uint32_t bytes);
void fglDestroyBuffer(FGLContext* gl, FGLBuffer b);
```

A buffer is up to 16 MiB of GPU memory. Index buffers hold `uint32_t`
indices. Everything a process creates is freed when it exits.

Vertex buffers hold `FGLVertex` by default:

| Struct | Floats | Attributes (location) |
| --- | --- | --- |
| `FGLVertex` | 10 | position xyz (0), normal xyz (1), color rgba (2) |
| `FGLVertexUV` | 12 | the same, plus texture coordinates uv (3) |

Call `fglVertexLayoutUV(gl)` before drawing `FGLVertexUV` data, and
`fglVertexLayoutDefault(gl)` to go back. For your own vertex struct,
describe where each attribute location sits:

```c
typedef struct { float x, y; float u, v; float shade; } MyVertex;

static const FGLAttribute attrs[] = {
    { offsetof(MyVertex, x), 2 },       // location 0: vec2 (z = 0, w = 1)
    { offsetof(MyVertex, u), 2 },       // location 1
    { offsetof(MyVertex, shade), 1 },   // location 2
};
fglVertexLayout(gl, sizeof(MyVertex), 3, attrs);
```

Offsets and the stride are in bytes and must be multiples of 4; a vertex
is at most 64 floats; there are 8 locations. Missing components read as
`(0, 0, 0, 1)`, so a 2-float position is a point at z = 0. The layout is
context state: it applies to every following draw, with shaders and with
the built-in programs (which read locations 0, 1 and 2).

In a shader, attributes get their location from `layout(location = N)`,
or from their name: `position`, `normal`, `color`, `uv` / `texcoord`
(also with `a_`, `in_`, `a` or `in` prefixes) are 0, 1, 2 and 3.
See [fgsl.md](fgsl.md#attributes).

## State and drawing

| Call | Effect |
| --- | --- |
| `fglClearColor(gl, r, g, b)` / `fglClear(gl, bits)` | Clear color and/or depth (`FGL_COLOR_BUFFER_BIT`, `FGL_DEPTH_BUFFER_BIT`). |
| `fglEnable` / `fglDisable` | `FGL_DEPTH_TEST`, `FGL_CULL_FACE` (back faces; counter-clockwise is front), `FGL_WIREFRAME`, `FGL_BLEND`. |
| `fglUseProgram(gl, p)` | Use a built-in program (and stop using a shader). |
| `fglUseShader(gl, sh)` | Use a shader. |
| `fglSetModel`, `fglSetView`, `fglSetProjection` | The usual three matrices. |
| `fglSetLight(gl, direction, color, ambient)` | One directional light; `direction` is where the light travels. |
| `fglSetMaterial(gl, specular, shininess)` | Highlight strength (0-1) and sharpness (1-128). |
| `fglViewport(gl, x, y, w, h)` | Area of the surface to draw into (defaults to all of it). |
| `fglDraw(gl, vertices, first, count)` | Triangles from consecutive vertices. |
| `fglDrawIndexed(gl, vertices, indices, first_index, count)` | Triangles from an index buffer. |
| `fglSubmit(gl)` | Run the recorded commands. |
| `fglPresent(gl)` | Submit and show the frame. Returns 0, or -1 with `gl->error` set. |
| `fglReadPixels(gl, out, pitch)` | Copy the frame into your memory. |

Calls only *record*; nothing is drawn until `fglSubmit`/`fglPresent`. State
changes between draws apply to the draws after them, in order, so you can
change a uniform, draw, change it again and draw again in one frame.

## Built-in programs

| Program | What it does |
| --- | --- |
| `FGL_PROGRAM_COLOR` | Vertex colors, no lighting. |
| `FGL_PROGRAM_GOURAUD` | Lighting per vertex (diffuse + Blinn-Phong highlight), interpolated. |
| `FGL_PROGRAM_PHONG` | The same lighting per pixel. |

They use `fglSetLight` and `fglSetMaterial`. They are faster than an
equivalent shader, so they remain a good choice for simple scenes.

## Shaders

```c
FGLShader* fglCreateShader(FGLContext* gl, const char* vertex_src, const char* fragment_src);
const char* fglShaderLog(FGLContext* gl);
void fglUseShader(FGLContext* gl, FGLShader* sh);
void fglDestroyShader(FGLContext* gl, FGLShader* sh);
```

`fglCreateShader` compiles both stages with the FGSL compiler
(`fglsl.h`), links them (varyings and uniforms are matched by name), and
loads the program onto the GPU. On failure it returns `NULL` and
`fglShaderLog(gl)` holds a message such as

```text
fragment shader line 12: unknown identifier 'normall'
```

The language is GLSL ES 1.0 / GLSL 1.30 with a few limits (no structs, no
`while`, loops must have constant bounds); [fgsl.md](fgsl.md) has the full
reference. The compiler runs inside your program and takes well under a
millisecond for typical shaders;
create shaders once at startup, not every frame.

### Uniforms

```c
int fglUniformLocation(FGLShader* sh, const char* name);    // -1 if not declared
void fglUniform1f(FGLShader* sh, int loc, float x);
void fglUniform2f(FGLShader* sh, int loc, float x, float y);
void fglUniform3f(FGLShader* sh, int loc, float x, float y, float z);
void fglUniform4f(FGLShader* sh, int loc, float x, float y, float z, float w);
void fglUniformVec3(FGLShader* sh, int loc, FGLVec3 v);
void fglUniformMatrix4(FGLShader* sh, int loc, FGLMat4 m);
void fglUniformArray(FGLShader* sh, int loc, uint32_t first, const float* values, uint32_t count);
void fglUniformTexture(FGLContext* gl, FGLShader* sh, int loc, FGLTexture t, uint32_t flags);
```

Uniform values belong to the shader and keep their value until you set
them again (also across frames and while another shader is in use).
Setting a location of `-1` does nothing, so you can skip checks.
`fglUniformArray` takes `count` elements packed tightly in the uniform's
type (`vec3` = 3 floats, `mat4` = 16).

### Automatic uniforms

Declare any of these and FarcadeGL keeps them up to date from the
context's state, so shaders work with `fglSetModel`, `fglSetLight` and
friends:

| Uniform | Type | Value |
| --- | --- | --- |
| `fgl_ModelViewProjection` | `mat4` | projection × view × model |
| `fgl_Model`, `fgl_View`, `fgl_Projection` | `mat4` | the matrices as set |
| `fgl_ModelView` | `mat4` | view × model |
| `fgl_NormalMatrix` | `mat4` | inverse transpose of the model matrix (use with `vec4(normal, 0.0)`) |
| `fgl_LightDirection` | `vec3` | `fglSetLight` direction (normalized; the way light travels) |
| `fgl_LightColor`, `fgl_Ambient` | `vec3` | `fglSetLight` colors |
| `fgl_CameraPosition` | `vec3` | the eye position, from the view matrix |
| `fgl_Material` | `vec2` | `fglSetMaterial` specular strength, shininess |

## Textures

```c
FGLTexture fglCreateTexture(FGLContext* gl, uint32_t width, uint32_t height, const uint32_t* pixels);
int fglTextureData(FGLContext* gl, FGLTexture t, const uint32_t* pixels, uint32_t pitch);
void fglDestroyTexture(FGLContext* gl, FGLTexture t);
void fglBindTexture(FGLContext* gl, uint32_t unit, FGLTexture t, uint32_t flags);
```

Textures are ARGB8888 (`0xAARRGGBB`), rows top to bottom, up to
4096 × 4096. In a shader, `texture(sampler, uv)` returns `vec4(r, g, b, a)`
in 0..1. `uv = (0, 0)` is the top-left corner of the image and `(1, 1)` the
bottom-right; `fglMakeCubeUV` maps each face upright.

Each `sampler2D` uniform gets a texture unit (0-3, in declaration order).
`fglUniformTexture(gl, sh, loc, tex, flags)` binds a texture to that
sampler's unit; `fglBindTexture` binds to a unit directly. Flags:

| Flag | Effect |
| --- | --- |
| `FGL_TEXTURE_LINEAR` | Bilinear filtering (default: nearest texel). |
| `FGL_TEXTURE_CLAMP` | Coordinates outside 0..1 use the edge texels (default: the texture repeats). |

Bindings are context state, like the vertex layout.

## Blending

`fglEnable(gl, FGL_BLEND)` mixes each new pixel with what is already
there using the fragment's alpha: `color = src.rgb * src.a + dst.rgb *
(1 - src.a)`. Draw opaque geometry first, then transparent geometry back
to front. With the built-in programs, alpha comes from the vertex color.

## Math

`FGLVec3`, `FGLMat4` and: `fglVec3`, `fglAdd`, `fglSub`, `fglScale3`,
`fglDot`, `fglCross`, `fglNormalize`, `fglIdentity`, `fglMul`,
`fglTranslate`, `fglScale`, `fglRotate(angle, axis)`,
`fglPerspective(fovy, aspect, near, far)`, `fglLookAt(eye, center, up)`,
`fglNormalMatrix`, and `fglSin`, `fglCos`, `fglTan`, `fglSqrt` (the SDK
has no libm).

`fglMakeCube(out, half_size, face_colors)` writes the 36 vertices of a
cube with one color per face (`NULL` for the default colors);
`fglMakeCubeUV` does the same with texture coordinates.

## Errors and limits

- `fglPresent`/`fglSubmit` return -1 and set `gl->error` to the first
  failed `SYS_GPU` result (`SYS_ERR_INVAL` for a bad command,
  `SYS_ERR_NOTFOUND` for a destroyed buffer or texture).
- `fglCreateShader` returns `NULL`; see `fglShaderLog`. If the kernel
  rejects a program the compiler produced, the reason is written to the
  serial log (`fgpu: shader program rejected: ...`).

| Limit | Value |
| --- | --- |
| Surface / texture size | 4096 × 4096 |
| Buffer size | 16 MiB |
| Command buffer | 1 MiB per submit (FarcadeGL grows its buffer as needed) |
| Shader program | 2048 instructions per stage, 32 temporary vec4s, 8 varyings, 64 uniform vec4s (a `mat4` uses 4), 256 constant vec4s, 4 texture units |
| Objects per process | 64 surfaces (contexts and textures), 128 buffers, 64 shaders, shared with other processes |

## Performance

The CPU backend runs on one core. Measured for the `fglcube` scene in a
640 × 480 window on a 2.8 GHz Xeon core: about 11 ms per frame with the
built-in Phong program and about 42 ms (24 fps) with `fglcube --shader`
(a 60-instruction fragment shader over about 180,000 pixels), so a full
shader costs roughly 4 times a built-in program. Fragment shaders run
8 pixels at a time with SSE. Under QEMU without KVM everything is many
times slower. Tips:

- Do per-vertex work in the vertex shader and pass it as a varying; the
  fragment shader runs for every pixel.
- Fewer varyings and fewer instructions are faster. `discard` makes the
  depth buffer update after the fragment shader instead of before it.
- Use the built-in programs for objects that need nothing special.

## Function reference

| Function | Section |
| --- | --- |
| `fglGetDeviceInfo` | [Contexts](#contexts) |
| `fglCreateContext`, `fglDestroyContext` | [Contexts](#contexts) |
| `fglCreateBuffer`, `fglBufferData`, `fglDestroyBuffer` | [Buffers](#buffers-and-vertex-layouts) |
| `fglVertexLayout`, `fglVertexLayoutUV`, `fglVertexLayoutDefault` | [Buffers](#buffers-and-vertex-layouts) |
| `fglEnable`, `fglDisable`, `fglViewport`, `fglClearColor`, `fglClear` | [State and drawing](#state-and-drawing) |
| `fglUseProgram` | [Built-in programs](#built-in-programs) |
| `fglSetModel`, `fglSetView`, `fglSetProjection`, `fglSetLight`, `fglSetMaterial` | [State and drawing](#state-and-drawing) |
| `fglDraw`, `fglDrawIndexed`, `fglSubmit`, `fglPresent`, `fglReadPixels` | [State and drawing](#state-and-drawing) |
| `fglCreateShader`, `fglShaderLog`, `fglUseShader`, `fglDestroyShader` | [Shaders](#shaders) |
| `fglUniformLocation`, `fglUniform1f` … `fglUniform4f`, `fglUniformVec3`, `fglUniformMatrix4`, `fglUniformArray`, `fglUniformTexture` | [Uniforms](#uniforms) |
| `fglCreateTexture`, `fglTextureData`, `fglDestroyTexture`, `fglBindTexture` | [Textures](#textures) |
| `fglMakeCube`, `fglMakeCubeUV` and the math functions | [Math](#math) |

## The FGPU interface underneath

`SYS_GPU` (40) takes a `syscall_gpu_request_t`. Operations: `INFO`,
`CREATE_SURFACE`, `DESTROY_SURFACE`, `CREATE_BUFFER`, `UPLOAD_BUFFER`,
`DESTROY_BUFFER`, `SUBMIT`, `READBACK`, `PRESENT`, `RELEASE_DISPLAY`, and
(ABI version 2) `CREATE_PROGRAM`, `DESTROY_PROGRAM`, `WRITE_SURFACE`.

A command buffer is a list of 32-bit words; each command starts with
`FGPU_CMD(opcode, length_in_words)`: `BIND_TARGET`, `VIEWPORT`, `CLEAR`,
`SET_STATE`, `SET_PROGRAM`, `SET_UNIFORMS`, `DRAW`, and (version 2)
`SET_SHADER`, `SET_CONSTANTS`, `SET_VERTEX_LAYOUT`, `BIND_TEXTURE`. Every
submit starts from default state.

A shader program is a blob of FGPU bytecode: a header, the vertex and
fragment instructions (five words each: opcode, destination, three
sources, with swizzles and negate/abs modifiers on vec4 registers) and a
constant pool. The instruction set and register files are described at
the end of `fgpu_abi.h`. Jumps only go forward, so every program finishes;
the kernel checks every instruction when the program is created. You can
produce blobs yourself, but `fglsl_compile` (`fglsl.h`) is the supported
way.
