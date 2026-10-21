#!/usr/bin/env python3
"""
Palette Lock render check over obs-websocket. Run it on the Windows VJ machine, against the real OBS.

STATUS: written against obsws-python 1.8.0 (its method signatures were checked).
Not yet run against a live OBS 32.2.2 with the graphics-context fix installed.

What it does:
  1. Builds a throwaway scene "PD-Test" that contains a mid-grey Color Source.
  2. Adds a Palette Lock filter with a known 2-colour palette (black -> pure red).
  3. Takes a screenshot of the scene and checks the centre pixel:
       A. strength 0  -> about grey (filter passes the image through)
       B. strength 1  -> red channel dominant, green and blue near 0, not black
       C. duotone     -> same expectation as B
  4. Deletes the scene at the end, unless --keep is given.

Setup (Windows):
  py -m pip install obsws-python pillow
  OBS: Tools > WebSocket Server Settings > Enable (port 4455). Put the password
  in the environment variable OBS_WS_PASSWORD. Never write it into a file or a commit.
  py obs_ws_check.py [--keep] [--host localhost] [--port 4455]

Exit code is 0 when every check passes. The script prints the raw pixels so you can reason about failures.

Colour space: Palette Lock keeps OBS_SOURCE_SRGB (option a). The shader converts the linear texel to
sRGB, maps in gamma space (as the mock does), and converts back to linear. Check D asserts the result:
palette #000000 -> #FF0000 on a #808080 source must give about #800000 (+-10 per channel).
"""
import argparse, base64, io, os, sys, time

try:
    import obsws_python as obs
    from PIL import Image
except ImportError:
    sys.exit("pip install obsws-python pillow")

SCENE, SRC, FLT = "PD-Test", "PD-Grey", "PD-Lock"
GREY_ABGR = 0xFF808080          # OBS colour ints are 0xAABBGGRR
BLACK_ABGR = 0xFF000000
RED_ABGR = 0xFF0000FF           # red lives in the LOW byte


GREY_ABGR_PAL = 0xFF808080
WHITE_ABGR = 0xFFFFFFFF


def filter_settings(strength, mode=0, top=RED_ABGR):
    # keys from src/palette-lock-filter.c (S_PALETTE / S_MODE / S_NCOLOURS / S_COLOUR)
    return {
        "n_palettes": 1, "live_palette": 0, "edit_palette": 0,
        "strength": float(strength), "contrast": 1.0, "preserve": 0.0, "crossfade_beats": 0.0,
        "palette.0.name": "TestRed", "palette.0.mode": mode, "palette.0.n_colours": 2,
        "palette.0.colour.0": BLACK_ABGR, "palette.0.colour.1": top,
    }


def centre_pixel(cl, name=SRC):
    # Screenshot the filtered SOURCE (its filter chain output), not the scene: the scene
    # canvas may be larger than the test source, leaving its centre transparent.
    r = cl.get_source_screenshot(name, "png", 64, 36, -1)
    data = r.image_data.split(",", 1)[1]
    im = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")
    return im.getpixel((im.width // 2, im.height // 2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=4455)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    cl = obs.ReqClient(host=a.host, port=a.port, password=os.environ.get("OBS_WS_PASSWORD", ""), timeout=10)

    scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
    if SCENE in scenes:
        cl.remove_scene(SCENE)
        time.sleep(0.3)
    cl.create_scene(SCENE)
    cl.create_input(SCENE, SRC, "color_source_v3", {"color": GREY_ABGR, "width": 1920, "height": 1080}, True)
    cl.create_source_filter(SRC, FLT, "palette_lock_filter", filter_settings(0.0))
    time.sleep(0.5)

    results = []

    def check(name, settings, pred, why):
        cl.set_source_filter_settings(SRC, FLT, settings, False)
        time.sleep(0.4)
        px = centre_pixel(cl)
        ok = pred(px)
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: centre pixel RGBA={px}  expect: {why}")

    check("A passthrough (strength 0)", filter_settings(0.0),
          lambda p: all(abs(c - 128) <= 40 for c in p[:3]), "about (128,128,128)")
    check("B gradient black->red (strength 1)", filter_settings(1.0, 0),
          lambda p: p[0] >= 40 and p[1] <= 25 and p[2] <= 25, "R clearly > 0, G and B near 0")
    check("C duotone black->red (strength 1)", filter_settings(1.0, 1),
          lambda p: p[0] >= 40 and p[1] <= 25 and p[2] <= 25, "R clearly > 0, G and B near 0")

    # D: colour-space check. luma(#808080) = 0.502 -> gradient black->red gives #800000 in gamma space.
    def near(p, rgb, tol=10):
        return all(abs(p[i] - rgb[i]) <= tol for i in range(3))

    check("D colour space #808080 -> #800000 (gradient, strength 1)", filter_settings(1.0, 0),
          lambda p: near(p, (128, 0, 0)), "(128,0,0) +-10; darker means texels are being mapped linearised")

    # F: discriminating colour-space check. #808080 -> #800000 alone cannot tell option (a) from
    # mapping linear texels directly (a 0->1 ramp commutes with the sRGB curve). Here the source is
    # white (l = 1) and the top colour is #808080: option (a) gives #808080; treating the palette hex
    # as linear would give about #BCBCBC.
    cl.set_input_settings(SRC, {"color": WHITE_ABGR}, True)
    check("F colour space white -> palette top #808080", filter_settings(1.0, 0, GREY_ABGR_PAL),
          lambda p: near(p, (128, 128, 128)), "(128,128,128) +-10; ~188 means palette is applied as linear")
    cl.set_input_settings(SRC, {"color": GREY_ABGR}, True)

    # E: alpha must survive (an alpha-0 output would show as black on the canvas)
    check("E output alpha opaque", filter_settings(1.0, 0), lambda p: p[3] >= 250, "alpha 255")

    if not a.keep:
        cl.remove_scene(SCENE)
        try:
            cl.remove_input(SRC)
        except Exception:
            pass
    print("ALL PASS" if all(results) else "SOME CHECKS FAILED")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
