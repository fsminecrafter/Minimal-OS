# FGSL shading language (`fglsl.h`)

FGSL (Farcade GL Shading Language) is the language FarcadeGL shaders are
written in. It is a subset of GLSL: if you know GLSL ES 1.0 (WebGL 1) or
desktop GLSL 1.30, you already know it. Shaders are compiled inside your
program by `fglsl.h` (FarcadeGL's `fglCreateShader` does this), into FGPU
bytecode that the kernel checks and runs.

```glsl
// vertex shader
uniform mat4 fgl_ModelViewProjection;
attribute vec3 position;
attribute vec2 uv;
varying vec2 v_uv;

void main() {
    v_uv = uv;
    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);
}
```

```glsl
// fragment shader
uniform sampler2D tex;
uniform float time;
varying vec2 v_uv;

void main() {
    vec4 c = texture(tex, v_uv);
    if (c.a < 0.1) discard;
    gl_FragColor = vec4(c.rgb * (0.8 + 0.2 * sin(time)), 1.0);
}
```

## Program structure

A shader is a list of global declarations and functions, with one
`void main()`. Both GLSL styles work:

| | GLSL ES 1.0 style | GLSL 1.30 style |
| --- | --- | --- |
| Vertex input | `attribute vec3 position;` | `in vec3 position;` |
| Vertex → fragment | `varying vec3 v_normal;` | `out vec3 v_normal;` / `in vec3 v_normal;` |
| Fragment output | `gl_FragColor = ...;` | `out vec4 color;` then `color = ...;` |

`#version`, `#extension` and other `#` lines are ignored, as are
`precision ...;` statements and the qualifiers `highp`, `mediump`,
`lowp`, `flat`, `smooth`, `centroid`, `invariant`. Comments are `//` and
`/* */`.

## Types

| Type | Notes |
| --- | --- |
| `float` | 32-bit float. |
| `vec2`, `vec3`, `vec4` | Float vectors. |
| `mat4` | 4 × 4 float matrix, column-major, as in GLSL. `m[i]` is column `i`. |
| `sampler2D` | A texture; uniforms and function parameters only. |
| `int`, `bool`, `uint`, `ivecN`, `bvecN` | Accepted, stored as floats. `true` is 1.0, `false` 0.0. Integer division is not truncated: use `floor()` or `int()`. |

`mat2`, `mat3`, structs and samplers other than `sampler2D` are not
supported. Arrays are supported for uniforms and local variables, with a
size up to 64 (uniforms) or 16 (locals), indexed by a constant or a loop
counter.

### Constructors

`vec4(1.0)`, `vec4(v3, 1.0)`, `vec3(v4)` (takes `.xyz`), `vec2(x, y)`,
`float(x)`, `int(x)` (truncates toward zero), `bool(x)`,
`mat4(c0, c1, c2, c3)` (four `vec4` columns) and `mat4(1.0)` (diagonal).
Constructors whose arguments are all constants are folded at compile time.

### Swizzles

`.xyzw`, `.rgba` and `.stpq`, in any order and with repeats when reading
(`v.zyx`, `c.rrr`); without repeats when writing (`v.xz = ...`).

## Variables

| Declaration | Meaning |
| --- | --- |
| `uniform T name;` | Set by the program with `fglUniform*`. The same name in both shaders is one uniform. |
| `attribute T name;` / `in T name;` (vertex) | Per-vertex input, `float` to `vec4`. |
| `varying T name;` / `out T name;` (vertex) | Output to the fragment shader, `float` to `vec4`, interpolated perspective-correctly. |
| `varying T name;` / `in T name;` (fragment) | Input from the vertex shader, matched by name and type. |
| `out vec4 name;` (fragment) | The fragment color (same as `gl_FragColor`). |
| `const T name = expr;` | A constant; folded when the value is known at compile time. |
| `T name;` (global or local) | A variable; starts at zero. |

### Attributes

Each attribute reads one location of the vertex layout (see
[farcadegl.md](farcadegl.md#buffers-and-vertex-layouts)). The location is
`layout(location = N)` if given, otherwise taken from the name:

| Name (also with an `a_`, `in_`, `a` or `in` prefix, any case) | Location | In `FGLVertex` / `FGLVertexUV` |
| --- | --- | --- |
| `position`, `pos`, `vertex` | 0 | `px, py, pz` |
| `normal` | 1 | `nx, ny, nz` |
| `color`, `colour` | 2 | `r, g, b, a` |
| `uv`, `texcoord`, `texcoord0` | 3 | `u, v` |

Other names get the first free location from 4. Components the layout
does not provide read as `0, 0, 0, 1`.

### Built-in variables

| Variable | Stage | Type | Meaning |
| --- | --- | --- | --- |
| `gl_Position` | vertex | `vec4` | Clip-space position (write). |
| `gl_FragColor` | fragment | `vec4` | Color, rgba in 0..1 (write). |
| `gl_FragCoord` | fragment | `vec4` | Pixel center (x + 0.5, y + 0.5, depth 0..1, 1/w). y grows downwards. |
| `gl_FrontFacing` | fragment | `bool` | True for counter-clockwise (front) triangles. |

FarcadeGL also fills the `fgl_*` uniforms when you declare them
(`fgl_ModelViewProjection`, `fgl_NormalMatrix`, `fgl_LightDirection`, ...):
see [farcadegl.md](farcadegl.md#automatic-uniforms).

## Statements

- Assignment `=`, `+=`, `-=`, `*=`, `/=`, and `++` / `--` (as statements).
- `if (...) ... else ...`
- `for (int i = A; i < B; i++) ...` — the start, end and step must be
  constants (`<`, `<=`, `>`, `>=`, `!=`; `i++`, `i--`, `++i`, `i += n`,
  `i -= n`). Loops are unrolled, at most 256 times. `break` and `continue`
  work. Inside the loop, `i` is a constant, so it can index arrays.
- `discard;` (fragment shader) drops the pixel.
- `return;` / `return expr;`
- `while` and `do` loops are not supported (every program must finish in a
  known number of steps).

## Expressions

Operators, from lowest to highest precedence: `?:`, `||`, `&&`, `==` `!=`,
`<` `>` `<=` `>=`, `+` `-`, `*` `/`, unary `-` `+` `!`, then calls, `[]`
and swizzles. Arithmetic works component by component; a `float` mixes with
any vector (`v * 2.0`). `mat4 * vec4`, `vec4 * mat4` and `mat4 * mat4` are
matrix products. Comparisons and `&&`, `||`, `!` work on scalars and give
0.0 or 1.0.

## Functions

User functions are supported and inlined at each call: parameters are
copies (`in`); `out` / `inout` parameters and recursion are not supported.
A function must be declared before `main` is compiled, in any order.

```glsl
float rim(vec3 n, vec3 v, float power) {
    return pow(1.0 - max(dot(n, v), 0.0), power);
}
```

### Built-in functions

`T` is `float` or a vector; arguments marked `float` may also be a single
float with vector arguments (as in GLSL).

| Function | Notes |
| --- | --- |
| `radians`, `degrees` | |
| `sin`, `cos`, `tan`, `atan(y, x)`, `atan(y_over_x)` | Error about 1e-5. |
| `pow`, `exp`, `exp2`, `log`, `log2`, `sqrt`, `inversesqrt` | `pow(x, y)` is 0 for `x <= 0` (GLSL leaves it undefined). |
| `abs`, `sign`, `floor`, `ceil`, `fract`, `mod(x, y)` | |
| `min`, `max`, `clamp(x, lo, hi)`, `mix(a, b, t)`, `step(edge, x)`, `smoothstep(e0, e1, x)` | |
| `length`, `distance`, `dot`, `cross`, `normalize`, `reflect(I, N)` | |
| `texture(sampler, uv)`, `texture2D(sampler, uv)` | Returns `vec4`, see [textures](farcadegl.md#textures). |

Not available: `asin`, `acos`, `refract`, `faceforward`, derivatives
(`dFdx`...), `texture` with a bias or LOD, matrix functions.

## Limits

| | Limit |
| --- | --- |
| Instructions per stage (after inlining and unrolling) | 2048 |
| Temporary registers (vec4) | 32 |
| Varyings | 8 |
| Uniform registers | 64 vec4 (`mat4` = 4, arrays count every element) |
| Constants | 256 vec4 |
| Samplers | 4 |
| Attributes | 8 |

The compiler reports when a shader goes over a limit
("shader too complex", "too many varyings ...").

## Errors

Errors name the stage and line:

```text
vertex shader line 7: unknown identifier 'positon'
fragment shader line 3: the vertex shader does not write varying 'v_uv'
fragment shader line 9: type mismatch in assignment
```

## Using the compiler directly

```c
#include "fglsl.h"

static fglsl_program_t prog;    // about 160 KiB: keep it static or malloc it
if (fglsl_compile(&prog, vertex_src, fragment_src) != 0)
    printf("%s\n", prog.error);
// prog.words / prog.size: the blob for SYS_GPU_CREATE_PROGRAM
// prog.uniforms[0 .. uniform_count-1]: name, type, first register (or unit)
// prog.attribs[0 .. attrib_count-1]: name, type, location
```

`fglsl.h` needs no libc, so it also builds on a Linux or macOS host, for
example to check shaders at build time. The bytecode format is described
in `fgpu_abi.h` ("Shader programs").
