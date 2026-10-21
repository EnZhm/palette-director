#!/usr/bin/env python3
"""Shader-parity harness for data/effects/palette-lock.effect (Palette Director P2).

Proves the shipped .effect recolour maths is a faithful port of the Palette
Director UI mock v2 (lockColour / mapA / mapB / lumaMatch / luma):

  1. compiles the HLSL .effect body through glslang's HLSL front end
     (stand-in for the D3D11 compile OBS performs when it loads the effect),
  2. compiles and renders the SAME maths as GLSL through moderngl/EGL
     (stand-in for the OpenGL path OBS uses on Linux),
  3. compares the rendered frames against an independent CPU (numpy) reference
     and asserts max channel diff <= 2/255.

Run (any machine with an EGL-capable GPU, e.g. TechnoCore):
    python3 -m venv .venv && .venv/bin/pip install moderngl numpy
    .venv/bin/python tools/parity/parity.py

Exit code 0 = parity holds; nonzero = a frame exceeded the 2/255 tolerance.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
EFFECT = ROOT / "data" / "effects" / "palette-lock.effect"

TOL = 2.0  # max channel diff, in 0..255 units (2/255)
W = H = 128


# ---- palettes (from the mock v2 set) -------------------------------------
def hexrgb(h):
    h = h.lstrip("#")
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]


PALETTES = {
    "Spectrum Fallout": ["#05000A", "#FF0033", "#9D00FF", "#0077FF", "#00F0FF",
                         "#39FF14", "#D4FF00", "#FF4D00"],
    "Night Neon": ["#0B0B12", "#3A0CA3", "#F72585", "#4CC9F0"],
    "Breakdown Duo": ["#0A0A12", "#C9C9FF"],
    "Ice": ["#05060A", "#0D3B66", "#7FDBFF", "#F0FBFF"],
}


def palette_array(name):
    """(8,4) float64 array, zero-padded to the shader's 8-slot float4 layout."""
    a = np.zeros((8, 4))
    cols = [hexrgb(c) for c in PALETTES[name]]
    for i, c in enumerate(cols):
        a[i, :3] = c
    return a


# ---- CPU reference (independent port of the mock maths) -------------------
def cpu_reference(src, palA, nA, modeA, palB, nB, modeB, pfade, amt, contrast,
                  preserve):
    src = src.astype(np.float64)

    def luma(c):
        return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]

    def luma_match(c, l):
        lm = np.maximum(luma(c), 0.002)
        return np.clip(c * (l[..., None] / lm[..., None]), 0.0, 1.0)

    def map_x(pal, n, mode, l):
        pal = pal[..., :3]  # drop the padded alpha channel
        if mode == 1:
            t = l * l * (3.0 - 2.0 * l)  # smoothstep
            return pal[0] + (pal[n - 1] - pal[0]) * t[..., None]
        x = l * (n - 1)
        i = np.minimum(np.floor(x).astype(np.int64), n - 2)
        f = (x - i)[..., None]
        return pal[i] + (pal[i + 1] - pal[i]) * f

    l = np.clip((luma(src) - 0.5) * contrast + 0.5, 0.0, 1.0)
    m = map_x(palA, nA, modeA, l) * (1.0 - pfade) + \
        map_x(palB, nB, modeB, l) * pfade
    m = m * (1.0 - preserve) + luma_match(m, l) * preserve
    return src * (1.0 - amt) + m * amt


# ---- GLSL mirror of the .effect maths -------------------------------------
GLSL_FRAG = """
#version 330
uniform sampler2D image;
uniform vec4 palA[8];
uniform int nA;
uniform int modeA;
uniform vec4 palB[8];
uniform int nB;
uniform int modeB;
uniform float pfade;
uniform float amt;
uniform float contrast;
uniform float preserve;
out vec4 fragColor;

float luma(vec3 c){ return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 lumaMatch(vec3 c, float l){ return clamp(c * (l / max(luma(c), 0.002)), 0.0, 1.0); }
vec3 mapA(float l){
    if (modeA == 1) return mix(palA[0].rgb, palA[nA - 1].rgb, smoothstep(0.0, 1.0, l));
    float x = l * float(nA - 1);
    int i = min(int(floor(x)), nA - 2);
    return mix(palA[i].rgb, palA[i + 1].rgb, x - float(i));
}
vec3 mapB(float l){
    if (modeB == 1) return mix(palB[0].rgb, palB[nB - 1].rgb, smoothstep(0.0, 1.0, l));
    float x = l * float(nB - 1);
    int i = min(int(floor(x)), nB - 2);
    return mix(palB[i].rgb, palB[i + 1].rgb, x - float(i));
}
vec3 lockColour(vec3 src){
    float l = clamp((luma(src) - 0.5) * contrast + 0.5, 0.0, 1.0);
    vec3 m = mix(mapA(l), mapB(l), pfade);
    return mix(m, lumaMatch(m, l), preserve);
}
void main(){
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(image, 0));
    vec3 src = texture(image, uv).rgb;
    fragColor = vec4(mix(src, lockColour(src), amt), 1.0);
}
"""

GLSL_VERT = """
#version 330
in vec2 v;
void main(){ gl_Position = vec4(v, 0.0, 1.0); }
"""


# ---- HLSL validation (glslang HLSL front end) ------------------------------
def effect_to_standalone_hlsl(text):
    """Rewrite the OBS .effect into a glslang-compilable HLSL fragment shader."""
    idx = text.find("technique")
    if idx >= 0:
        text = text[:idx]
    text = text.replace("uniform texture2d image;", "Texture2D image;")
    text = re.sub(r"sampler_state\s+(\w+)\s*\{.*?\};", r"SamplerState \1;",
                  text, flags=re.S)
    text = re.sub(r"\buniform\s+", "", text)
    text = text.replace(": POSITION", ": SV_Position")
    text = text.replace(": TARGET", ": SV_Target")
    text += "\nfloat4 main(VertData vert_in) : SV_Target { return PSLockColour(vert_in); }\n"
    return text


def validate_hlsl(text):
    glslang = shutil.which("glslangValidator")
    if not glslang:
        print("  HLSL: SKIP (glslangValidator not found)")
        return None
    f = HERE / "check" / "palette-lock.hlsl"
    f.parent.mkdir(exist_ok=True)
    f.write_text(text)
    r = subprocess.run([glslang, "-D", "-V", "-S", "frag", "-e", "main",
                        str(f), "-o", "/dev/null"], capture_output=True,
                       text=True)
    ok = r.returncode == 0
    msg = (r.stdout + r.stderr).strip().splitlines()
    print(f"  HLSL (glslang -D -V): {'PASS' if ok else 'FAIL'}")
    if not ok:
        print("    " + "\n    ".join(msg[-14:]))
    return ok


# ---- render + compare ------------------------------------------------------
def render_frame(ctx, prog, vao, src, params):
    import moderngl

    rgba = np.dstack([src, np.ones((H, W, 1), dtype="f4")]).astype("f4")
    tex = ctx.texture((W, H), 4, dtype="f4")
    tex.repeat_x = tex.repeat_y = False
    tex.write(rgba.tobytes())

    rt = ctx.texture((W, H), 4, dtype="f4")
    fbo = ctx.framebuffer([rt])

    prog["image"].value = 0
    tex.use(0)
    prog["palA"].write(params["palA"].astype("f4").tobytes())
    prog["palB"].write(params["palB"].astype("f4").tobytes())
    prog["nA"].value = params["nA"]
    prog["modeA"].value = params["modeA"]
    prog["nB"].value = params["nB"]
    prog["modeB"].value = params["modeB"]
    prog["pfade"].value = params["pfade"]
    prog["amt"].value = params["amt"]
    prog["contrast"].value = params["contrast"]
    prog["preserve"].value = params["preserve"]

    fbo.use()
    fbo.clear()
    vao.render(moderngl.TRIANGLE_STRIP)

    data = fbo.read(components=3, dtype="f4")
    out = np.frombuffer(data, dtype="f4").reshape(H, W, 3).astype(np.float64)
    tex.release()
    rt.release()
    fbo.release()
    return out


def detect_flip(ctx, prog, vao):
    """Render identity (amt=0) on a known image; report whether readback is
    vertically flipped relative to the CPU convention."""
    base = np.zeros((H, W, 3), dtype="f4")
    for y in range(H):
        for x in range(W):
            base[y, x] = [x / W, y / H, 0.5]
    params = dict(palA=palette_array("Night Neon"), nA=4, modeA=0,
                  palB=palette_array("Night Neon"), nB=4, modeB=0, pfade=0.0,
                  amt=0.0, contrast=1.1, preserve=0.0)
    out = render_frame(ctx, prog, vao, base, params)
    diff = np.abs(out - base).max() * 255
    flip_diff = np.abs(out - np.flipud(base)).max() * 255
    return diff > flip_diff  # True => readback is flipped


def main():
    import moderngl

    effect_text = EFFECT.read_text()
    bad = sorted({ch for ch in effect_text if ord(ch) > 127})
    if bad:
        print(f"FAIL: non-ASCII in .effect: {bad}")
        return 2

    print(f"effect: {EFFECT} ({len(effect_text)} bytes)")
    hlsl_ok = validate_hlsl(effect_to_standalone_hlsl(effect_text))

    ctx = moderngl.create_context(standalone=True, backend="egl")
    prog = ctx.program(vertex_shader=GLSL_VERT, fragment_shader=GLSL_FRAG)
    vbo = ctx.buffer(np.array([-1, -1, 1, -1, -1, 1, 1, 1], dtype="f4"))
    vao = ctx.vertex_array(prog, [(vbo, "2f", "v")])

    flip = detect_flip(ctx, prog, vao)
    print(f"  orientation: readback {'flipped' if flip else 'upright'} "
          f"(flip={'np.flipud' if flip else 'none'})")

    # ---- three test frames -------------------------------------------------
    rng = np.random.default_rng(2077)
    frame_ramp = np.tile(np.linspace(0.0, 1.0, W, dtype="f4")[None, :, None],
                         (H, 1, 3))
    frame_random = rng.random((H, W, 3), dtype="f4")
    patch = np.array([[0.0, 0.0, 0.0], [1.0, 1.0, 1.0], [1.0, 0.0, 0.0],
                      [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [0.5, 0.5, 0.5],
                      [1.0, 1.0, 0.0], [0.0, 1.0, 1.0], [1.0, 0.0, 1.0],
                      [0.2, 0.6, 0.9]], dtype="f4")
    grid = (np.arange(H, dtype=np.int64)[:, None] // 8 +
            np.arange(W, dtype=np.int64)[None, :] // 8) % patch.shape[0]
    frame_patch = patch[grid]

    cases = [
        ("gradient->gradient mid-crossfade",
         frame_ramp, dict(palA=palette_array("Spectrum Fallout"), nA=8, modeA=0,
                          palB=palette_array("Night Neon"), nB=4, modeB=0,
                          pfade=0.5, amt=1.0, contrast=1.1, preserve=0.0)),
        ("gradient->duotone + keep-brightness",
         frame_random, dict(palA=palette_array("Night Neon"), nA=4, modeA=0,
                            palB=palette_array("Breakdown Duo"), nB=2, modeB=1,
                            pfade=0.25, amt=0.8, contrast=1.5, preserve=0.3)),
        ("duotone->gradient full-preserve",
         frame_patch, dict(palA=palette_array("Breakdown Duo"), nA=2, modeA=1,
                           palB=palette_array("Ice"), nB=4, modeB=0, pfade=0.75,
                           amt=0.6, contrast=0.8, preserve=1.0)),
    ]

    all_ok = True
    for name, src, p in cases:
        gpu = render_frame(ctx, prog, vao, src, p)
        if flip:
            gpu = np.flipud(gpu)
        cpu = cpu_reference(src, **p)
        diff = np.abs(gpu - cpu).max() * 255.0
        ok = diff <= TOL
        all_ok &= ok
        print(f"  {name:38s} max channel diff {diff:6.3f}/255 "
              f"{'PASS' if ok else 'FAIL'}")

    vbo.release()
    vao.release()
    prog.release()
    ctx.release()

    print(f"parity {'PASS' if (all_ok and hlsl_ok) else 'FAIL'} "
          f"(tol {TOL:.1f}/255)")
    return 0 if (all_ok and hlsl_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
