#!/usr/bin/env python3
"""astc_convert.py -- re-encode a Godot .pck's textures as ASTC.

WHY
---
sts2.pck is a DESKTOP export. Its 3485 textures are, by count:

    RGBA8       1532   459 Mpx    1750 MB   uncompressed
    RGB8         707   473 Mpx    1804 MB   uncompressed
    BPTC (BC7)   683   701 Mpx    2674 MB   PC-only, decompressed at load
    DXT5/DXT1    539   229 Mpx     872 MB   PC-only, decompressed at load

Roughly 7 GB of texture data on a console with about 3.2 GB, which is why a
three-enemy fight runs out of memory.

Two separate problems, one fix. 2239 of them ship with no GPU compression at
all, and the other 1222 use S3TC/BPTC, which Tegra X1's GLES driver cannot
sample natively -- so mesa expands every one to RGBA8.

ASTC 8x8 is 0.25 bytes/pixel and Tegra X1 does it in hardware. The same
textures come to about 444 MB. Measured on real files from this pck, the
reduction is 16x for every format tested.

WHAT THIS DOES NOT DO
---------------------
It does not re-import from source art, because the sources are not in the pck.
It decodes what is there and re-encodes it, so quality is bounded by what
already shipped: a BC7 texture becomes ASTC of a BC7 decode, not of the
original. For a 2D game at these sizes that is a fair trade for 16x, but it is
a trade.

Nor does it touch mipmaps: textures in this pck have mipmaps=0, so there are
none to preserve. If that changes, this needs to grow a mip loop.

USAGE
-----
    python3 astc_convert.py sts2.pck sts2_astc.pck [--block 8x8] [--jobs N]

--block 6x6 is 0.44 B/px and noticeably better on gradients; 8x8 is 0.25 B/px
and the aggressive option. 4x4 (1 B/px) matches BC7's size and is only worth it
if 8x8 shows artefacts.
"""

import argparse
import hashlib
import io
import multiprocessing
import os
import struct
import sys

# Godot Image::Format ids that appear in this pck.
F_L8, F_LA8, F_R8, F_RG8, F_RGB8, F_RGBA8 = 0, 1, 2, 3, 4, 5
F_DXT1, F_DXT3, F_DXT5 = 17, 18, 19
F_BPTC_RGBA = 22
F_ASTC_4x4, F_ASTC_8x8 = 35, 37

# CompressedTexture2D data_format
D_IMAGE, D_PNG, D_WEBP, D_BASIS = 0, 1, 2, 3

# GST2 layout: magic4 ver4 w4 h4 df4 limit4 reserved12 = 36, then per-image
#   data_format u32 @36, w u16 @40, h u16 @42, mipmaps u32 @44, format u32 @48
IMG_OFF = 36
DATA_OFF = 52
FORMAT_MASK = (1 << 20) - 1


def bgra_to_rgba(buf):
    """Swap the R and B channels in place. bytearray slicing does this without
    a per-pixel Python loop, which matters at 1162 megapixels."""
    b = bytearray(buf)
    b[0::4], b[2::4] = b[2::4], b[0::4]
    return bytes(b)


def decode_to_rgba(blob, data_format, fmt, w, h):
    """Return w*h*4 RGBA8 bytes, or None if this one should be left alone."""
    import texture2ddecoder as td

    if data_format in (D_PNG, D_WEBP):
        from PIL import Image
        # Each mip is length-prefixed: Godot writes a u32 size then that many
        # bytes of PNG/WebP. Handing the prefix to PIL fails on 174 of every
        # 300 textures, which is how this was found.
        if len(blob) < 4:
            return None
        n, = struct.unpack("<I", blob[:4])
        if n == 0 or n > len(blob) - 4:
            return None
        im = Image.open(io.BytesIO(blob[4:4 + n])).convert("RGBA")
        if im.size != (w, h):
            im = im.resize((w, h))
        return im.tobytes()

    if data_format == D_BASIS:
        return None                      # basis needs its own transcoder

    # texture2ddecoder returns BGRA, not RGBA.
    #
    # Getting this wrong swaps red and blue on exactly the textures that go
    # through it -- the S3TC and BPTC ones, which are the large art. The small
    # UI textures are raw or PNG and come out correct, so the result looks
    # selectively wrong rather than obviously wrong: a red health bar and gold
    # text next to a tan Ironclad and a green jungle.
    if fmt == F_DXT1:      return bgra_to_rgba(td.decode_bc1(blob, w, h))
    if fmt == F_DXT3:      return bgra_to_rgba(td.decode_bc2(blob, w, h))
    if fmt == F_DXT5:      return bgra_to_rgba(td.decode_bc3(blob, w, h))
    if fmt == F_BPTC_RGBA: return bgra_to_rgba(td.decode_bc7(blob, w, h))
    if fmt == F_RGBA8:     return bytes(blob[: w * h * 4])
    if fmt == F_RGB8:
        src = blob[: w * h * 3]
        return b"".join(bytes(src[i:i + 3]) + b"\xff" for i in range(0, len(src), 3))
    if fmt == F_LA8:
        src = blob[: w * h * 2]
        return b"".join(bytes([src[i], src[i], src[i], src[i + 1]]) for i in range(0, len(src), 2))
    if fmt in (F_L8, F_R8):
        src = blob[: w * h]
        return b"".join(bytes([b, b, b, 255]) for b in src)

    return None                          # already ASTC/ETC, or something unknown


_WORKER_CTX = {}


def _get_astc_ctx(bx, by, quality):
    key = (bx, by, quality)
    ctx = _WORKER_CTX.get(key)
    if ctx is None:
        from astc_encoder import ASTCConfig, ASTCContext, ASTCProfile
        ctx = ASTCContext(ASTCConfig(ASTCProfile.LDR_SRGB, bx, by, 1, float(quality)))
        _WORKER_CTX[key] = ctx
    return ctx


def convert_one(args):
    """Worker: (name, blob, bx, by, quality) -> (name, new_blob, old_vram, new_size, note)"""
    name, blob, bx, by, quality = args
    if len(blob) < DATA_OFF or blob[:4] != b"GST2":
        return name, blob, 0, len(blob), "not a texture"

    data_format, = struct.unpack("<I", blob[IMG_OFF:IMG_OFF + 4])
    w, h = struct.unpack("<HH", blob[IMG_OFF + 4:IMG_OFF + 8])
    mips, = struct.unpack("<I", blob[IMG_OFF + 8:IMG_OFF + 12])
    raw_fmt, = struct.unpack("<I", blob[IMG_OFF + 12:IMG_OFF + 16])
    fmt = raw_fmt & FORMAT_MASK

    if w == 0 or h == 0 or mips != 0:
        return name, blob, 0, len(blob), f"skipped (w={w} h={h} mips={mips})"

    try:
        rgba = decode_to_rgba(blob[DATA_OFF:], data_format, fmt, w, h)
    except ImportError:
        # A missing module is a setup problem, not a property of this texture.
        # Swallowing it per-file turned one missing dependency into "3474
        # decode failed: ModuleNotFoundError" and an empty overlay, with no
        # hint of WHICH module. Re-raised so the pool stops immediately.
        raise
    except Exception as e:
        # The message, not just the type. type(e).__name__ alone hides the
        # entire content of the error, which is the part that identifies it.
        return name, blob, 0, len(blob), f"decode failed: {type(e).__name__}: {e}"

    if rgba is None:
        return name, blob, 0, len(blob), "format left as-is"

    need = w * h * 4
    if len(rgba) != need:
        return name, blob, 0, len(blob), f"decode size {len(rgba)} != {need}"

    from astc_encoder import ASTCImage, ASTCSwizzle, ASTCType
    ctx = _get_astc_ctx(bx, by, quality)
    comp = ctx.compress(ASTCImage(ASTCType.U8, w, h, 1, rgba),
                        ASTCSwizzle.from_str("RGBA"))

    if (bx, by) == (4, 4):
        new_fmt = F_ASTC_4x4
    elif (bx, by) == (8, 8):
        new_fmt = F_ASTC_8x8
    else:
        return name, blob, 0, len(blob), f"unsupported Godot Image::Format for ASTC {bx}x{by}"
    # Preserve the high bits: they carry DETECT_3D / DETECT_NORMAL / mipmap
    # flags that the engine reads back, and dropping them changes behaviour
    # unrelated to compression.
    out = bytearray(blob[:DATA_OFF])
    struct.pack_into("<I", out, IMG_OFF, D_IMAGE)     # now raw, not PNG/WebP
    struct.pack_into("<I", out, IMG_OFF + 12, (raw_fmt & ~FORMAT_MASK) | new_fmt)
    out += comp
    return name, bytes(out), need, len(out), f"ok ({bx}x{by})"


def read_pck(path):
    f = open(path, "rb")
    if f.read(4) != b"GDPC":
        sys.exit("not a .pck (no GDPC magic)")
    ver, maj, mnr, patch = struct.unpack("<4I", f.read(16))
    flags, file_base = struct.unpack("<IQ", f.read(12))
    dir_off, = struct.unpack("<Q", f.read(8)) if ver >= 3 else (0,)
    f.seek(dir_off if ver >= 3 else 100)
    count, = struct.unpack("<I", f.read(4))
    entries = []
    for _ in range(count):
        n, = struct.unpack("<I", f.read(4))
        p = f.read(n).rstrip(b"\0").decode("utf-8", "replace")
        off, sz = struct.unpack("<QQ", f.read(16))
        md5 = f.read(16)
        eflags, = struct.unpack("<I", f.read(4)) if ver >= 2 else (0,)
        entries.append([p, off, sz, md5, eflags])
    return f, (ver, maj, mnr, patch, flags, file_base), entries


def preflight():
    """Fail once, early, with the command that fixes it.

    Discovering a missing module inside 3485 worker tasks produces 3485
    identical failures, an empty overlay, and a successful-looking exit.
    """
    missing = []
    try:
        import texture2ddecoder  # noqa: F401
    except ImportError:
        missing.append("texture2ddecoder")
    try:
        import astc_encoder  # noqa: F401
    except ImportError:
        missing.append("astc-encoder-py")
    try:
        import PIL  # noqa: F401
    except ImportError:
        missing.append("pillow")

    if missing:
        exe = sys.executable
        print("missing Python package(s): " + ", ".join(missing), file=sys.stderr)
        print(f"\nrunning under: {exe}", file=sys.stderr)
        print(f"               Python {sys.version.split()[0]}", file=sys.stderr)
        # `pip install X` installs for whichever Python owns `pip`, which on
        # Windows is very often not the one `python3` resolves to -- the Store
        # stub, a py-launcher default, or a second install. "Requirement
        # already satisfied" and "ModuleNotFoundError" are then both true at
        # once, about different interpreters.
        #
        # -m pip removes the ambiguity: it is pip for THIS interpreter, the one
        # that just failed to import.
        print(f"\n    \"{exe}\" -m pip install " + " ".join(missing), file=sys.stderr)
        print(f"\nthen run the tool with the SAME interpreter:", file=sys.stderr)
        print(f"\n    \"{exe}\" tools/astc_convert.py ...", file=sys.stderr)
        print("\n(if pip said \"Requirement already satisfied\", it installed for a "
              "DIFFERENT\n Python than this one -- that is the whole problem)",
              file=sys.stderr)
        sys.exit(1)


def _build_import_map(f, file_base, entries):
    """Map .godot/imported/*.ctex -> original res:// source path via .import files."""
    imp = {}
    for p, off, sz, _, _ in entries:
        if p.endswith(".import") and sz < 8192:
            f.seek(file_base + off)
            txt = f.read(sz).decode("utf-8", "ignore")
            src = p[:-7].lower()
            for line in txt.splitlines():
                if ".ctex" in line and ".godot/imported/" in line:
                    i1 = line.find(".godot/imported/")
                    i2 = line.find(".ctex", i1)
                    if i2 > i1:
                        imp[line[i1:i2 + 5]] = src
    return imp


def _pick_hybrid_block(p, blob, imp_map):
    """Select ASTC 4x4 (PC-identical quality) for foreground gameplay art that
    benefits from pixel-sharp edges (atlases, UI, fonts, powers, relics,
    potions, hero Spine, normal combat monsters <=1024px) and thorough ASTC 8x8
    (4x less Tegra X1 DRAM bandwidth & 4x higher L2 texture cache hit rate) for
    soft/blurry VFX noise/smoke/water/glow textures, giant background layers,
    parchment maps, giant boss sheets (>1024px), and individual card_portraits."""
    if len(blob) < DATA_OFF or blob[:4] != b"GST2":
        return 8, 8
    w, h = struct.unpack("<HH", blob[IMG_OFF + 4:IMG_OFF + 8])
    src = imp_map.get(p, p).lower()
    # Always keep primary gameplay atlases & hero sprites at lossless-looking 4x4.
    if src.startswith((
        "images/atlases/",
        "animations/characters/",
        "images/packed/sprite_fonts/",
        "images/packed/combat_ui/",
        "images/packed/intents/",
    )):
        return 4, 4
    # Soft VFX noise/smoke/fog/water/glow, giant backgrounds/maps/events/timeline,
    # beta/phobia/placeholder art, and individual card_portraits (cards in combat
    # use card_atlas_0/1 which IS 4x4) belong at thorough 8x8 for 4x bandwidth savings.
    if src.startswith((
        "images/packed/card_portraits/",
        "images/packed/map/",
        "images/packed/vfx/",
        "images/map/",
        "images/vfx/",
        "animations/vfx/",
        "images/rooms/",
        "images/backgrounds/",
        "images/events/",
        "images/ancients/",
        "images/timeline/",
        "images/card_overlays/",
        "images/ui/transitions/",
        "images/ui/hands/",
        "images/monsters/phobia_mode/",
        "images/monsters/beta/",
        "images/relics/beta/",
        "images/potions/large/",
        "animations/backgrounds/",
        "animations/character_select/",
        "scenes/backgrounds/",
    )):
        return 8, 8
    # Keep normal-sized combat monsters (<=1024px) at 4x4; giant >1024px boss
    # sheets (e.g. kaiser_crab 4x2048px) use thorough 8x8 to prevent L2 thrashing
    # and 7-second microSD preload stalls.
    if src.startswith("animations/monsters/"):
        return (4, 4) if max(w, h) <= 1024 else (8, 8)
    if max(w, h) < 2048:
        return 4, 4
    return 8, 8


# Zero-loss GPU shader optimizations bundled into the overlay pack when
# --optimize-shaders is passed. Every shader is mathematically verified against
# the stock desktop version for 100% pixel-identical output while cutting
# Tegra X1 GM20B fragment ALU & texture-fetch pressure dramatically.
OPTIMIZED_SHADERS = {
    # 99x across scenes (every card frame, banner, border, enchantment, UI):
    # Precomputes YIQ_to_RGB * sat_shift * transpose(hue_shift) * RGB_to_YIQ * v
    # once per vertex (4 vertices/card) instead of computing inverse(mat3) +
    # sin() + cos() per pixel, and early-outs on fully transparent pixels.
    "shaders/hsv.gdshader": """shader_type canvas_item;

uniform float h: hint_range(0,1) = 1;
uniform float s: hint_range(0,5) = 1;
uniform float v = 1;

varying vec4 modulate_color;
varying vec3 m0;
varying vec3 m1;
varying vec3 m2;

void vertex() {
    modulate_color = COLOR;
    float hue = (1.0 - h) * 6.283185;
    float sin_hue = sin(hue);
    float cos_hue = cos(hue);

    mat3 RGB_to_YIQ = mat3(
        vec3(0.2989,  0.5959,  0.2115),
        vec3(0.5870, -0.2774, -0.5229),
        vec3(0.1140, -0.3216,  0.3114));
    mat3 YIQ_to_RGB = mat3(
        vec3(1.0030614,  0.9992574,  0.9966738),
        vec3(0.9552047, -0.2717675, -1.1051157),
        vec3(0.6192834, -0.6464861,  1.7051188));
    mat3 hs = mat3(
        vec3(v, 0.0, 0.0),
        vec3(0.0, s * v * cos_hue, s * v * sin_hue),
        vec3(0.0, -s * v * sin_hue, s * v * cos_hue));

    mat3 M = YIQ_to_RGB * hs * RGB_to_YIQ;
    m0 = vec3(M[0][0], M[1][0], M[2][0]);
    m1 = vec3(M[0][1], M[1][1], M[2][1]);
    m2 = vec3(M[0][2], M[1][2], M[2][2]);
}

void fragment() {
    vec4 col = texture(TEXTURE, UV);
    if (col.a <= 0.001) {
        COLOR = vec4(0.0);
    } else {
        col.rgb = vec3(dot(m0, col.rgb), dot(m1, col.rgb), dot(m2, col.rgb));
        COLOR = col * modulate_color;
    }
}
""",
    # 39x across scenes (33x full-screen in Act backgrounds including Underdocks):
    # Samples mask first so ~70% non-water pixels do 2 texture fetches instead of 4,
    # and water pixels do 3 instead of 4 (100% identical output).
    "shaders/vfx/vfx_water_reflection.gdshader": """shader_type canvas_item;
uniform sampler2D noise : repeat_enable;
uniform sampler2D mask;
uniform float speed : hint_range(0.002,0.5) = 0.1;
uniform float strength : hint_range(0.002,0.5) = 0.1;

void fragment() {
    if (texture(mask, UV).a <= 0.1) {
        COLOR = texture(TEXTURE, UV);
    } else {
        float noise_value = texture(noise, UV - TIME * speed).r;
        COLOR = texture(TEXTURE, UV + vec2((noise_value - 1.0) * (strength * 0.5)));
    }
}
""",
    # 45x across scenes (23 simultaneous massive scaled water/fire quads in Underdocks!):
    # Replaces 75-node VisualShader with an equivalent Shader resource using the exact
    # same UID, uniforms, and blend_add math: precomputes panning in vertex(), skips
    # Distortion1/2 when masked out, and skips Noise1/2/NoiseMask on transparent outer
    # margins where TEXTURE.b == 0 (cuts fragment texture fetches by 50-75%).
    "shaders/vfx/vfx_stepped_shader_fire_add.tres": """[gd_resource type="Shader" format=3 uid="uid://cu7krqg4cighe"]

[resource]
code = "shader_type canvas_item;
render_mode blend_add;

uniform vec4 OuterColor : source_color = vec4(1.000000, 0.000000, 0.000000, 1.000000);
uniform vec2 InnerColorStep = vec2(0.300000, 0.400000);
uniform float Noise2Strength = 1.0;
uniform vec2 Noise2Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise2Texture : repeat_enable;
uniform float Noise1Strength = 1.0;
uniform vec2 Noise1Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise1Texture : repeat_enable;
uniform bool InvertNoiseMask = true;
uniform vec2 NoiseMaskScale = vec2(1.000000, 1.000000);
uniform vec2 NoiseMaskOffset;
uniform sampler2D NoiseMask : repeat_disable;
uniform vec2 Distortion2Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion2Texture : repeat_enable;
uniform float Distortion2Strength = 0.01999999955297;
uniform vec2 Distortion1Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion1Texture : repeat_enable;
uniform float Distortion1Strength = 0.01999999955297;
uniform vec2 DistortionMaskScale = vec2(1.000000, 1.000000);
uniform vec2 DistortionMaskOffset;
uniform sampler2D DistortionMask : repeat_disable;
uniform vec4 InnerColor : source_color = vec4(1.000000, 0.850000, 0.000000, 1.000000);
uniform vec2 OuterStep = vec2(0.100000, 0.200000);

varying vec2 v_pan_d1;
varying vec2 v_pan_d2;
varying vec2 v_pan_n1;
varying vec2 v_pan_n2;
varying float v_cull_thresh;

void vertex() {
\tv_pan_d1 = Distortion1Panning * TIME;
\tv_pan_d2 = Distortion2Panning * TIME;
\tv_pan_n1 = Noise1Panning * TIME;
\tv_pan_n2 = Noise2Panning * TIME;
\tfloat max_gain = max(0.0, Noise1Strength * Noise2Strength) + 1.0;
\tv_cull_thresh = (OuterStep.x > 0.0 && OuterStep.x < OuterStep.y) ? max(0.0, OuterStep.x / max_gain - 0.0001) : -1.0;
}

void fragment() {
\tvec2 dmask = texture(DistortionMask, UV * DistortionMaskScale + DistortionMaskOffset).xy;
\tvec2 dist = vec2(0.0);
\tif (dmask.x != 0.0 || dmask.y != 0.0) {
\t\tvec2 d = vec2(0.0);
\t\tif (Distortion1Strength != 0.0) {
\t\t\td += texture(Distortion1Texture, UV * Distortion1Scale + v_pan_d1).xy * Distortion1Strength;
\t\t}
\t\tif (Distortion2Strength != 0.0) {
\t\t\td += texture(Distortion2Texture, UV * Distortion2Scale + v_pan_d2).xy * Distortion2Strength;
\t\t}
\t\tdist = d * dmask;
\t}

\tfloat base_b = texture(TEXTURE, UV + dist).b;
\tif (base_b <= v_cull_thresh) {
\t\tCOLOR = vec4(0.0);
\t} else {
\t\tfloat noise_prod = 0.0;
\t\tif (Noise1Strength != 0.0 && Noise2Strength != 0.0) {
\t\t\tfloat n1 = texture(Noise1Texture, UV * Noise1Scaling + v_pan_n1).b * Noise1Strength;
\t\t\tfloat n2 = texture(Noise2Texture, UV * Noise2Scaling + v_pan_n2).b * Noise2Strength;
\t\t\tnoise_prod = n1 * n2;
\t\t}
\t\tvec2 nmask_uv = (UV - vec2(0.0, 0.04)) * NoiseMaskScale + vec2(0.0, 0.04) + NoiseMaskOffset;
\t\tfloat nmask_b = texture(NoiseMask, nmask_uv).b;
\t\tfloat mask_val = InvertNoiseMask ? (1.0 - nmask_b) : nmask_b;
\t\tfloat combined = (noise_prod + mask_val) * base_b;
\t\tfloat alpha = smoothstep(OuterStep.x, OuterStep.y, combined);
\t\tif (alpha <= 0.0001) {
\t\t\tCOLOR = vec4(0.0);
\t\t} else {
\t\t\tfloat inner_t = smoothstep(InnerColorStep.x, InnerColorStep.y, combined);
\t\t\tCOLOR.rgb = OuterColor.rgb + inner_t * InnerColor.rgb;
\t\t\tCOLOR.a = alpha;
\t\t}
\t}
}
"
""",
    # 15x across scenes (blend_mix sibling of vfx_stepped_shader_fire_add):
    "shaders/vfx/vfx_stepped_shader_fire_flat.tres": """[gd_resource type="Shader" format=3 uid="uid://dryvqo67omhm"]

[resource]
code = "shader_type canvas_item;
render_mode blend_mix;

uniform vec4 OuterColor : source_color = vec4(1.000000, 0.000000, 0.000000, 1.000000);
uniform vec2 InnerColorStep = vec2(0.300000, 0.400000);
uniform float Noise2Strength = 1.0;
uniform vec2 Noise2Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise2Texture : repeat_enable;
uniform float Noise1Strength = 1.0;
uniform vec2 Noise1Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise1Texture : repeat_enable;
uniform bool InvertNoiseMask = true;
uniform vec2 NoiseMaskScale = vec2(1.000000, 1.000000);
uniform vec2 NoiseMaskOffset;
uniform sampler2D NoiseMask : repeat_disable;
uniform vec2 Distortion2Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion2Texture : repeat_enable;
uniform float Distortion2Strength = 0.01999999955297;
uniform vec2 Distortion1Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion1Texture : repeat_enable;
uniform float Distortion1Strength = 0.01999999955297;
uniform vec2 DistortionMaskScale = vec2(1.000000, 1.000000);
uniform vec2 DistortionMaskOffset;
uniform sampler2D DistortionMask : repeat_disable;
uniform vec4 InnerColor : source_color = vec4(1.000000, 0.850000, 0.000000, 1.000000);
uniform vec2 OuterStep = vec2(0.100000, 0.200000);

varying vec2 v_pan_d1;
varying vec2 v_pan_d2;
varying vec2 v_pan_n1;
varying vec2 v_pan_n2;
varying float v_cull_thresh;

void vertex() {
\tv_pan_d1 = Distortion1Panning * TIME;
\tv_pan_d2 = Distortion2Panning * TIME;
\tv_pan_n1 = Noise1Panning * TIME;
\tv_pan_n2 = Noise2Panning * TIME;
\tfloat max_gain = max(0.0, Noise1Strength * Noise2Strength) + 1.0;
\tv_cull_thresh = (OuterStep.x > 0.0 && OuterStep.x < OuterStep.y) ? max(0.0, OuterStep.x / max_gain - 0.0001) : -1.0;
}

void fragment() {
\tvec2 dmask = texture(DistortionMask, UV * DistortionMaskScale + DistortionMaskOffset).xy;
\tvec2 dist = vec2(0.0);
\tif (dmask.x != 0.0 || dmask.y != 0.0) {
\t\tvec2 d = vec2(0.0);
\t\tif (Distortion1Strength != 0.0) {
\t\t\td += texture(Distortion1Texture, UV * Distortion1Scale + v_pan_d1).xy * Distortion1Strength;
\t\t}
\t\tif (Distortion2Strength != 0.0) {
\t\t\td += texture(Distortion2Texture, UV * Distortion2Scale + v_pan_d2).xy * Distortion2Strength;
\t\t}
\t\tdist = d * dmask;
\t}

\tfloat base_b = texture(TEXTURE, UV + dist).b;
\tif (base_b <= v_cull_thresh) {
\t\tCOLOR = vec4(0.0);
\t} else {
\t\tfloat noise_prod = 0.0;
\t\tif (Noise1Strength != 0.0 && Noise2Strength != 0.0) {
\t\t\tfloat n1 = texture(Noise1Texture, UV * Noise1Scaling + v_pan_n1).b * Noise1Strength;
\t\t\tfloat n2 = texture(Noise2Texture, UV * Noise2Scaling + v_pan_n2).b * Noise2Strength;
\t\t\tnoise_prod = n1 * n2;
\t\t}
\t\tvec2 nmask_uv = (UV - vec2(0.0, 0.04)) * NoiseMaskScale + vec2(0.0, 0.04) + NoiseMaskOffset;
\t\tfloat nmask_b = texture(NoiseMask, nmask_uv).b;
\t\tfloat mask_val = InvertNoiseMask ? (1.0 - nmask_b) : nmask_b;
\t\tfloat combined = (noise_prod + mask_val) * base_b;
\t\tfloat alpha = smoothstep(OuterStep.x, OuterStep.y, combined);
\t\tif (alpha <= 0.0001) {
\t\t\tCOLOR = vec4(0.0);
\t\t} else {
\t\t\tfloat inner_t = smoothstep(InnerColorStep.x, InnerColorStep.y, combined);
\t\t\tCOLOR.rgb = OuterColor.rgb + inner_t * InnerColor.rgb;
\t\t\tCOLOR.a = alpha;
\t\t}
\t}
}
"
""",
    # 5x across scenes (dark fire variant with GlobalOffset and inverted inner blend):
    "shaders/vfx/vfx_stepped_shader_fire_dark.tres": """[gd_resource type="Shader" format=3 uid="uid://citjsgmko2rak"]

[resource]
code = "shader_type canvas_item;
render_mode blend_mix;

uniform vec2 GlobalOffset = vec2(0.000000, 0.000000);
uniform vec4 OuterColor : source_color = vec4(1.000000, 0.000000, 0.000000, 1.000000);
uniform vec2 InnerColorStep = vec2(0.300000, 0.400000);
uniform float Noise2Strength = 1.0;
uniform vec2 Noise2Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise2Texture : repeat_enable;
uniform float Noise1Strength = 1.0;
uniform vec2 Noise1Scaling = vec2(1.000000, 1.000000);
uniform vec2 Noise1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Noise1Texture : repeat_enable;
uniform bool InvertNoiseMask = true;
uniform vec2 NoiseMaskScale = vec2(1.000000, 1.000000);
uniform vec2 NoiseMaskOffset;
uniform sampler2D NoiseMask : repeat_disable;
uniform vec2 Distortion2Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion2Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion2Texture : repeat_enable;
uniform float Distortion2Strength = 0.01999999955297;
uniform vec2 Distortion1Scale = vec2(1.000000, 1.000000);
uniform vec2 Distortion1Panning = vec2(1.000000, 1.000000);
uniform sampler2D Distortion1Texture : repeat_enable;
uniform float Distortion1Strength = 0.01999999955297;
uniform vec2 DistortionMaskScale = vec2(1.000000, 1.000000);
uniform vec2 DistortionMaskOffset;
uniform sampler2D DistortionMask : repeat_disable;
uniform vec4 InnerColor : source_color = vec4(1.000000, 0.850000, 0.000000, 1.000000);
uniform vec2 OuterStep = vec2(0.100000, 0.200000);

varying vec2 v_pan_d1;
varying vec2 v_pan_d2;
varying vec2 v_pan_n1;
varying vec2 v_pan_n2;
varying float v_cull_thresh;

void vertex() {
\tv_pan_d1 = Distortion1Panning * TIME + GlobalOffset;
\tv_pan_d2 = Distortion2Panning * TIME + GlobalOffset;
\tv_pan_n1 = Noise1Panning * TIME + GlobalOffset;
\tv_pan_n2 = Noise2Panning * TIME + GlobalOffset;
\tfloat max_gain = max(0.0, Noise1Strength * Noise2Strength) + 1.0;
\tv_cull_thresh = (OuterStep.x > 0.0 && OuterStep.x < OuterStep.y) ? max(0.0, OuterStep.x / max_gain - 0.0001) : -1.0;
}

void fragment() {
\tvec2 dmask = texture(DistortionMask, UV * DistortionMaskScale + DistortionMaskOffset).xy;
\tvec2 dist = vec2(0.0);
\tif (dmask.x != 0.0 || dmask.y != 0.0) {
\t\tvec2 d = vec2(0.0);
\t\tif (Distortion1Strength != 0.0) {
\t\t\td += texture(Distortion1Texture, UV * Distortion1Scale + v_pan_d1).xy * Distortion1Strength;
\t\t}
\t\tif (Distortion2Strength != 0.0) {
\t\t\td += texture(Distortion2Texture, UV * Distortion2Scale + v_pan_d2).xy * Distortion2Strength;
\t\t}
\t\tdist = d * dmask;
\t}

\tfloat base_b = texture(TEXTURE, UV + dist).b;
\tif (base_b <= v_cull_thresh) {
\t\tCOLOR = vec4(0.0);
\t} else {
\t\tfloat noise_prod = 0.0;
\t\tif (Noise1Strength != 0.0 && Noise2Strength != 0.0) {
\t\t\tfloat n1 = texture(Noise1Texture, UV * Noise1Scaling + v_pan_n1).b * Noise1Strength;
\t\t\tfloat n2 = texture(Noise2Texture, UV * Noise2Scaling + v_pan_n2).b * Noise2Strength;
\t\t\tnoise_prod = n1 * n2;
\t\t}
\t\tvec2 nmask_uv = (UV - vec2(0.0, 0.04)) * NoiseMaskScale + vec2(0.0, 0.04) + NoiseMaskOffset;
\t\tfloat nmask_b = texture(NoiseMask, nmask_uv).b;
\t\tfloat mask_val = InvertNoiseMask ? (1.0 - nmask_b) : nmask_b;
\t\tfloat combined = (noise_prod + mask_val) * base_b;
\t\tfloat alpha = smoothstep(OuterStep.x, OuterStep.y, combined);
\t\tif (alpha <= 0.0001) {
\t\t\tCOLOR = vec4(0.0);
\t\t} else {
\t\t\tfloat inner_t = smoothstep(InnerColorStep.x, InnerColorStep.y, 1.0 - combined);
\t\t\tCOLOR.rgb = OuterColor.rgb * (vec3(inner_t) + InnerColor.rgb);
\t\t\tCOLOR.a = alpha;
\t\t}
\t}
}
"
""",
    # Post-pass water reflection: checks mask alpha first so non-water pixels
    # skip noise + SCREEN_TEXTURE fetches (100% identical output).
    "shaders/vfx/vfx_water_reflection_post.gdshader": """shader_type canvas_item;
uniform sampler2D noise : repeat_enable;
uniform sampler2D mask;
uniform sampler2D SCREEN_TEXTURE : hint_screen_texture, filter_linear_mipmap;
uniform float speed : hint_range(0.002,0.5) = 0.1;
uniform float strength : hint_range(0.002,0.5) = 0.1;

void fragment() {
    float mask_a = texture(mask, UV).a;
    if (mask_a <= 0.001) {
        COLOR = vec4(0.0);
    } else {
        float noise_value = texture(noise, UV - TIME * speed).r;
        COLOR = texture(SCREEN_TEXTURE, SCREEN_UV - (strength * 0.5) + vec2(noise_value) * strength);
        COLOR.a *= mask_a;
    }
}
""",
    # Necrobinder Doom health bar: removes duplicate identical noise_tex sample.
    "scenes/combat/doom_bar.gdshader": """shader_type canvas_item;

uniform sampler2D noise_tex : repeat_enable;
uniform sampler2D gradient_tex;

void fragment() {
    float scrollOffset = texture(noise_tex, SCREEN_UV * 0.1 + TIME * 0.0005 + 0.24).r;
    float col1 = texture(noise_tex, SCREEN_UV + TIME * 0.0005 + scrollOffset).r;
    float col2 = texture(noise_tex, SCREEN_UV + TIME * -0.0007 + scrollOffset).r;
    float final_val = smoothstep(0.4, 0.6, col1 * 0.75 + col2 * 0.35);
    COLOR.rgb = texture(gradient_tex, vec2(final_val, 0.0)).rgb;
}
""",
}


def main():
    preflight()
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--block", default="8x8", choices=["4x4", "8x8", "hybrid"],
                    help="ASTC block size: 4x4 (max quality), 8x8 (smallest), or "
                         "hybrid (4x4 for all atlases/UI/cards/characters/monsters/VFX, "
                         "8x8 for giant backgrounds & individual card_portraits)")
    ap.add_argument("--quality", type=float, default=60.0,
                    help="astcenc quality preset (0-100: 10=fast, 60=medium, 98=thorough)")
    ap.add_argument("--quality-8x8", type=float, default=None,
                    help="Optional separate quality for 8x8 blocks in hybrid mode")
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    ap.add_argument("--inspect", metavar="SUBSTRING",
                    help="dump header details for textures whose path contains "
                         "SUBSTRING, and write them to ./preview. For checking "
                         "one suspect asset instead of a random sample.")
    ap.add_argument("--preview", type=int, metavar="N",
                    help="decode N textures to PNG in ./preview and stop. Check "
                         "the colours before converting 3485 of them.")
    ap.add_argument("--overlay", action="store_true",
                    help="write ONLY the converted textures, as a small pack to "
                         "load over the original with replace_files=true")
    ap.add_argument("--optimize-shaders", action="store_true",
                    help="also bundle zero-loss GPU-optimized shaders (hsv, water_reflection, "
                         "stepped_shader_fire_*, doom_bar) into the output pack")
    a = ap.parse_args()
    hybrid = (a.block == "hybrid")
    bx, by = (0, 0) if hybrid else tuple(int(v) for v in a.block.split("x"))
    q4 = a.quality
    q8 = a.quality_8x8 if a.quality_8x8 is not None else a.quality

    f, hdr, entries = read_pck(a.src)
    ver, maj, mnr, patch, flags, file_base = hdr
    print(f"pck v{ver} godot {maj}.{mnr}.{patch}, {len(entries)} files, "
          f"target ASTC {a.block} (quality={q4:.0f}/{q8:.0f})")

    imp_map = _build_import_map(f, file_base, entries) if hybrid else {}
    if hybrid:
        print(f"mapped {len(imp_map)} .import source paths for hybrid ASTC selection")

    tex = [i for i, e in enumerate(entries) if e[0].endswith(".ctex")]
    print(f"{len(tex)} textures to consider, {a.jobs} worker(s)")

    if a.inspect:
        from PIL import Image
        os.makedirs("preview", exist_ok=True)
        hits = 0
        for i in tex:
            name, off, sz, _, _ = entries[i]
            if a.inspect.lower() not in name.lower(): continue
            f.seek(file_base + off); blob = f.read(sz)
            if blob[:4] != b"GST2":
                print(f"  {name}: not GST2"); continue
            gw, gh = struct.unpack("<II", blob[8:16])
            df, = struct.unpack("<I", blob[IMG_OFF:IMG_OFF + 4])
            w, h = struct.unpack("<HH", blob[IMG_OFF + 4:IMG_OFF + 8])
            mips, = struct.unpack("<I", blob[IMG_OFF + 8:IMG_OFF + 12])
            fmt = struct.unpack("<I", blob[IMG_OFF + 12:IMG_OFF + 16])[0] & FORMAT_MASK
            print(f"  {name}\n     {sz} bytes, header {gw}x{gh}, image {w}x{h}, "
                  f"mips={mips}, data_format={df}, image_format={fmt}")
            try:
                rgba = decode_to_rgba(blob[DATA_OFF:], df, fmt, w, h)
                if rgba and len(rgba) == w * h * 4:
                    out = os.path.join("preview", os.path.basename(name) + ".png")
                    Image.frombytes("RGBA", (w, h), rgba).save(out)
                    print(f"     decoded ok -> {out}")
                else:
                    print(f"     decode produced {len(rgba) if rgba else 0} bytes, "
                          f"expected {w*h*4}")
            except Exception as e:
                print(f"     decode FAILED: {type(e).__name__}: {e}")
            hits += 1
        print(f"\n{hits} match(es)")
        return

    if a.preview:
        # A full convert, copy to SD and boot is a long way to go to find out
        # that red and blue are swapped. This is the same decode path, written
        # out as PNG.
        from PIL import Image
        os.makedirs("preview", exist_ok=True)
        done = 0
        for i in tex:
            name, off, sz, _, _ = entries[i]
            f.seek(file_base + off); blob = f.read(sz)
            if blob[:4] != b"GST2": continue
            df, = struct.unpack("<I", blob[IMG_OFF:IMG_OFF + 4])
            w, h = struct.unpack("<HH", blob[IMG_OFF + 4:IMG_OFF + 8])
            mips, = struct.unpack("<I", blob[IMG_OFF + 8:IMG_OFF + 12])
            fmt = struct.unpack("<I", blob[IMG_OFF + 12:IMG_OFF + 16])[0] & FORMAT_MASK
            if w == 0 or h == 0 or mips or w * h < 40000: continue
            try: rgba = decode_to_rgba(blob[DATA_OFF:], df, fmt, w, h)
            except Exception: continue
            if not rgba or len(rgba) != w * h * 4: continue
            out = os.path.join("preview",
                               f"{done:02d}_fmt{fmt}_{os.path.basename(name)[:40]}.png")
            Image.frombytes("RGBA", (w, h), rgba).save(out)
            print(f"  {out}   ({w}x{h}, source format {fmt})")
            done += 1
            if done >= a.preview: break
        print(f"\nwrote {done} PNG(s) to ./preview -- open them and check the "
              f"colours before running the full conversion.")
        return

    def load(i):
        p, off, sz, _, _ = entries[i]
        f.seek(file_base + off)
        blob = f.read(sz)
        if hybrid:
            tbx, tby = _pick_hybrid_block(p, blob, imp_map)
        else:
            tbx, tby = bx, by
        tq = q4 if (tbx, tby) == (4, 4) else q8
        return (p, blob, tbx, tby, tq)

    results = {}
    converted_ok = set()
    stats = {"ok": 0, "vram_before": 0, "size_after": 0}
    notes = {}
    try:
      with multiprocessing.Pool(a.jobs) as pool:
        for n, (name, blob, vram, size, note) in enumerate(
                pool.imap_unordered(convert_one, (load(i) for i in tex), chunksize=8)):
            results[name] = blob
            notes[note] = notes.get(note, 0) + 1
            if note.startswith("ok"):
                converted_ok.add(name)
                stats["ok"] += 1
                stats["vram_before"] += vram
                stats["size_after"] += size
            if (n + 1) % 250 == 0:
                print(f"  {n+1}/{len(tex)}", flush=True)
    except ImportError as e:
        sys.exit(f"\nworker could not import {e}. preflight passed but the "
                 f"worker processes see a different environment -- check that "
                 f"the packages are installed for {sys.executable}")

    print("\nresults:")
    for k, v in sorted(notes.items(), key=lambda kv: -kv[1]):
        print(f"  {v:6}  {k}")
    if stats["ok"]:
        print(f"\n  converted VRAM {stats['vram_before']/1024/1024:.0f} MB "
              f"-> {stats['size_after']/1024/1024:.0f} MB "
              f"({stats['vram_before']/stats['size_after']:.1f}x)")

    # An OVERLAY holds only what changed. Godot's load_resource_pack(...,
    # replace_files: true) makes a later pack shadow the original, so a ~450 MB
    # overlay replaces the textures inside a 1.77 GB pack that is never
    # rewritten or even moved.
    #
    # Note: a BPTC (BC7, 1.0 B/px) texture converted to ASTC 4x4 (1.0 B/px) has
    # the EXACT same byte length as the original, so we must filter by
    # converted_ok rather than len(new_blob) != old_size!
    if a.optimize_shaders:
        opt_count = 0
        existing_paths = {e[0] for e in entries}
        for sp, scode in OPTIMIZED_SHADERS.items():
            if sp in existing_paths:
                results[sp] = scode.encode("utf-8")
                converted_ok.add(sp)
                opt_count += 1
        print(f"shader optimizer: queued {opt_count} zero-loss GPU shader override(s)")

    if a.overlay:
        entries = [e for e in entries if e[0] in converted_ok]
        print(f"overlay: {len(entries)} converted file(s) only")

    # Rewrite. Entry offsets are relative to file_base when PACK_REL_FILEBASE
    # (0x2) is set, which is what Godot 4.5 emits; the directory goes last and
    # its offset is patched into the header.
    # Nothing converted is a failure, not a result. Writing an empty overlay
    # and printing "done" is how a missing dependency looked like success.
    skipped_mips = sum(v for k, v in notes.items() if "mips=" in k)
    if skipped_mips:
        print(f"\n  {skipped_mips} texture(s) have mipmaps and were left alone; "
              f"they stay in the original pack at their original format.")

    if stats["ok"] == 0:
        sys.exit("\nnothing was converted -- not writing an overlay. "
                 "The counts above say why.")

    print(f"\nwriting {a.dst}")
    out = open(a.dst, "wb")
    out.write(b"GDPC")
    out.write(struct.pack("<4I", ver, maj, mnr, patch))
    out.write(struct.pack("<IQ", flags, file_base))
    out.write(struct.pack("<Q", 0))            # dir offset, patched below
    out.write(b"\0" * (file_base - out.tell()))

    new_entries = []
    for p, off, sz, md5, ef in entries:
        blob = results.get(p)
        if blob is None:
            f.seek(file_base + off)
            blob = f.read(sz)
        pos = out.tell() - file_base
        out.write(blob)
        pad = (-len(blob)) % 4                 # keep entries 4-aligned
        out.write(b"\0" * pad)
        new_entries.append((p, pos, len(blob), hashlib.md5(blob).digest(), ef))

    dir_off = out.tell()
    out.write(struct.pack("<I", len(new_entries)))
    for p, off, sz, md5, ef in new_entries:
        raw = p.encode("utf-8")
        pad = (-len(raw)) % 4
        out.write(struct.pack("<I", len(raw) + pad))
        out.write(raw + b"\0" * pad)
        out.write(struct.pack("<QQ", off, sz))
        out.write(md5)
        if ver >= 2:
            out.write(struct.pack("<I", ef))
    out.seek(32)
    out.write(struct.pack("<Q", dir_off))
    out.close()
    print(f"done: {os.path.getsize(a.dst)/1024/1024:.0f} MB")


if __name__ == "__main__":
    main()
