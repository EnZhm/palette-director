# Shader parity harness

Proves `data/effects/palette-lock.effect` is a faithful port of the Palette
Director UI mock v2 recolour maths (`lockColour` / `mapA` / `mapB` /
`lumaMatch` / `luma`):

1. compiles the HLSL `.effect` body with glslang's HLSL front end (stand-in for
   the D3D11 compile OBS performs at load time),
2. compiles and renders the same maths as GLSL through moderngl/EGL (stand-in
   for OBS's OpenGL path on Linux),
3. compares three rendered test frames against an independent CPU (numpy)
   reference and asserts the max channel diff is <= 2/255.

## Run

Requires an EGL-capable GPU (TechnoCore has one) and `moderngl` + `numpy`.

    python3 -m venv .venv
    .venv/bin/pip install moderngl numpy
    .venv/bin/python tools/parity/parity.py

Exit code 0 means parity holds. The output block is pasted into the PR.
