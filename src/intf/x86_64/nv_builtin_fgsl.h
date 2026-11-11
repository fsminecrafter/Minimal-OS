#pragma once

/*
 * FGPU's built-in programs (vertex color, Gouraud, Phong) as FGSL source,
 * for hardware backends: the CPU backend has them as C code
 * (fgpu_raster.c, light()), a GPU needs shaders. Compiled in the kernel
 * with the SDK's fglsl.h the first time a program is drawn.
 *
 * Uniforms mirror fgpu_uniforms_t. Varyings: world position, world
 * normal, color.
 */

#define NV_BUILTIN_UNIFORMS \
    "uniform mat4 u_mvp; uniform mat4 u_model; uniform mat4 u_normal;\n" \
    "uniform vec3 u_light_dir; uniform vec3 u_light_color; uniform vec3 u_ambient;\n" \
    "uniform vec3 u_camera; uniform vec2 u_material;\n"

#define NV_BUILTIN_LIGHT \
    "vec3 lit(vec3 n_in, vec3 pos, vec3 base) {\n" \
    "  vec3 n = normalize(n_in);\n" \
    "  vec3 l = normalize(-u_light_dir);\n" \
    "  float diff = max(dot(n, l), 0.0);\n" \
    "  float spec = 0.0;\n" \
    "  if (u_material.x > 0.0 && diff > 0.0) {\n" \
    "    vec3 h = normalize(l + normalize(u_camera - pos));\n" \
    "    float nh = dot(n, h);\n" \
    "    if (nh > 0.0) spec = pow(nh, clamp(floor(u_material.y), 1.0, 128.0)) * u_material.x;\n" \
    "  }\n" \
    "  return base * (u_ambient + u_light_color * diff) + u_light_color * spec;\n" \
    "}\n"

#define NV_BUILTIN_VS_HEAD \
    NV_BUILTIN_UNIFORMS \
    "attribute vec3 position; attribute vec3 normal; attribute vec4 color;\n" \
    "varying vec3 v_world; varying vec3 v_normal; varying vec4 v_color;\n"

#define NV_BUILTIN_VS_BODY(color_expr) \
    "void main() {\n" \
    "  vec4 p = vec4(position, 1.0);\n" \
    "  gl_Position = u_mvp * p;\n" \
    "  v_world = (u_model * p).xyz;\n" \
    "  v_normal = (u_normal * vec4(normal, 0.0)).xyz;\n" \
    "  v_color = " color_expr ";\n" \
    "}\n"

#define NV_BUILTIN_FS_HEAD \
    NV_BUILTIN_UNIFORMS \
    "varying vec3 v_world; varying vec3 v_normal; varying vec4 v_color;\n"

// Vertex colors (FGPU_PROGRAM_COLOR) and the vertex stage of Phong.
#define NV_BUILTIN_VS_PLAIN   NV_BUILTIN_VS_HEAD NV_BUILTIN_VS_BODY("color")
// Lighting per vertex (FGPU_PROGRAM_GOURAUD).
#define NV_BUILTIN_VS_GOURAUD NV_BUILTIN_VS_HEAD NV_BUILTIN_LIGHT \
    NV_BUILTIN_VS_BODY("vec4(lit(v_normal, v_world, color.rgb), color.a)")
#define NV_BUILTIN_FS_PLAIN   NV_BUILTIN_FS_HEAD "void main() { gl_FragColor = v_color; }\n"
// Lighting per pixel (FGPU_PROGRAM_PHONG).
#define NV_BUILTIN_FS_PHONG   NV_BUILTIN_FS_HEAD NV_BUILTIN_LIGHT \
    "void main() { gl_FragColor = vec4(lit(v_normal, v_world, v_color.rgb), v_color.a); }\n"

static const char* const nv_builtin_uniform_names[8] = {
    "u_mvp", "u_model", "u_normal", "u_light_dir", "u_light_color", "u_ambient", "u_camera",
    "u_material",
};

/*
 * The last stage of the motion video codec on the GPU: 4:2:0 YCbCr
 * planes (three one-byte textures) to RGB. The arithmetic is
 * mmc_to_argb's (mosmedia_motion.h) on whole numbers held in floats, so
 * the pixels are exactly the CPU decoder's. v_px is the pixel position.
 */
#define NV_VIDEO_VS \
    "attribute vec2 position; layout(location = 1) attribute vec2 pixel; varying vec2 v_px;\n" \
    "void main() { v_px = pixel; gl_Position = vec4(position, 0.0, 1.0); }\n"

#define NV_VIDEO_FS \
    "uniform sampler2D ty; uniform sampler2D tu; uniform sampler2D tv;\n" \
    "uniform vec4 u_inv; uniform vec2 u_cmax; varying vec2 v_px;\n" \
    "float q(float s) { return floor(s * 255.0 + 0.5); }\n" \
    "void main() {\n" \
    "  vec2 p = floor(v_px);\n" \
    "  vec2 c = floor(p * 0.5);\n" \
    "  vec2 c2 = clamp(c + (p - c * 2.0) * 2.0 - 1.0, vec2(0.0, 0.0), u_cmax);\n" \
    "  vec2 a = (c + 0.5) * u_inv.zw;\n" \
    "  vec2 b = (c2 + 0.5) * u_inv.zw;\n" \
    "  float lum = q(texture(ty, (p + 0.5) * u_inv.xy).r);\n" \
    "  float u = floor((9.0 * q(texture(tu, a).r) + 3.0 * q(texture(tu, vec2(b.x, a.y)).r)\n" \
    "    + 3.0 * q(texture(tu, vec2(a.x, b.y)).r) + q(texture(tu, b).r) + 8.0) * 0.0625);\n" \
    "  float v = floor((9.0 * q(texture(tv, a).r) + 3.0 * q(texture(tv, vec2(b.x, a.y)).r)\n" \
    "    + 3.0 * q(texture(tv, vec2(a.x, b.y)).r) + q(texture(tv, b).r) + 8.0) * 0.0625);\n" \
    "  float cb = u - 128.0;\n" \
    "  float cr = v - 128.0;\n" \
    "  vec3 rgb = vec3(lum + floor((359.0 * cr + 128.0) * 0.00390625),\n" \
    "                  lum - floor((88.0 * cb + 183.0 * cr + 128.0) * 0.00390625),\n" \
    "                  lum + floor((454.0 * cb + 128.0) * 0.00390625));\n" \
    "  gl_FragColor = vec4(clamp(rgb, 0.0, 255.0) * 0.00392156862745098, 1.0);\n" \
    "}\n"
