#!/usr/bin/env python3
"""Make the launcher's box art from a scan of the 1997 magazine ad.

usage: tools/make_boxart.py [ad image] [--out-dir assets/img] [--font FILE]

Writes boxart_ja.tga, the ad as it was, and boxart_en.tga, with the Japanese title and tagline
frosted over and English set in the ad's style, in Noto Sans (SIL Open Font License; any bold
sans will do, see --font). Also boxart.tga, a copy of the English one: at startup the game
copies whichever matches the chosen language over it, as that is the file the launcher shows.

Keeps the title and Ayumi: crops away the banner, the contest strip and the side copy,
then paints out what's left of the copy beside the title. Needs ffmpeg for AVIF.
"""
import argparse
import pathlib
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

# Fractions of the ad, so that any scan of it works.
CROP = (0.158, 0.070, 0.815, 0.728)
# The third column of side copy, which runs down beside the title, inside that crop.
PAINT_OUT = [(0.770, 0.340, 0.830, 0.690)]
BLEND_PIXELS = 14
OUTPUT_HEIGHT = 720


def load(path):
    if path.suffix.lower() == ".avif":
        with tempfile.TemporaryDirectory() as tmp:
            png = pathlib.Path(tmp) / "ad.png"
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(path), "-frames:v", "1", str(png)], check=True)
            return Image.open(png).convert("RGB")
    return Image.open(path).convert("RGB")


def paint_out(image, box):
    """Replace a strip with a smear of what's beside it, blended in at the edges."""
    w, h = image.size
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    donor = image.crop((max(0, 2 * x0 - x1), y0, x0, y1)).transpose(Image.FLIP_LEFT_RIGHT)
    donor = donor.resize((x1 - x0, y1 - y0)).filter(ImageFilter.GaussianBlur(5))
    mask = Image.new("L", image.size, 0)
    mask.paste(255, (x0, y0, x1, y1))
    mask = mask.filter(ImageFilter.GaussianBlur(BLEND_PIXELS / 2))
    layer = image.copy()
    layer.paste(donor, (x0, y0))
    return Image.composite(layer, image, mask)


# Where the Japanese title sits in the finished box art, as fractions: tagline, logo, subtitle.
TITLE_AREA = (-0.02, 0.168, 1.02, 0.428)
TAGLINE = "The Famicom Disk System classic returns with a new story!"
TITLE = "BS Detective Club"
SUBTITLE = "~ The Past That Disappeared in the Snow ~"
FONT_DEFAULT = "/usr/share/fonts/google-noto/NotoSans-ExtraCondensedBlack.ttf"
FONT_ITALIC = "/usr/share/fonts/google-noto/NotoSans-SemiBoldItalic.ttf"
FONT_TAGLINE = "/usr/share/fonts/google-noto/NotoSans-ExtraBoldItalic.ttf"


def frost(art, box):
    """Blur the title area into a soft haze, lighter towards the middle, faded in at its edges."""
    w, h = art.size
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    haze = art.filter(ImageFilter.GaussianBlur(26))
    haze = Image.blend(haze, Image.new("RGB", art.size, (232, 240, 252)), 0.72)
    mask = Image.new("L", art.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((x0, y0, x1, y1), radius=12, fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(14))
    return Image.composite(haze, art, mask)


def outlined(size, text, font, fill_top, fill_bottom, rings, depth=0, depth_colour=None):
    """Text with a vertical fill gradient inside rings of outline, widest first, and an extrusion."""
    pad = sum(r for r, _ in rings) + depth + 4
    probe = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=font)
    tw, th = probe[2] - probe[0], probe[3] - probe[1]
    canvas = Image.new("RGBA", (tw + 2 * pad, th + 2 * pad), (0, 0, 0, 0))
    glyphs = Image.new("L", canvas.size, 0)
    ImageDraw.Draw(glyphs).text((pad - probe[0], pad - probe[1]), text, font=font, fill=255)
    total = sum(r for r, _ in rings)
    if depth:
        solid = glyphs.filter(ImageFilter.MaxFilter(2 * total + 1))
        for d in range(depth, 0, -1):
            canvas.paste(Image.new("RGBA", canvas.size, depth_colour), (0, 0), ImageChops.offset(solid, d, d))
    reach = total
    for radius, colour in rings:
        grown = glyphs.filter(ImageFilter.MaxFilter(2 * reach + 1)) if reach else glyphs
        canvas.paste(Image.new("RGBA", canvas.size, colour), (0, 0), grown)
        reach -= radius
    gradient = Image.new("RGBA", canvas.size)
    top, bottom = pad, pad + th
    for y in range(canvas.height):
        t = min(1.0, max(0.0, (y - top) / max(1, bottom - top)))
        c = tuple(int(a + (b - a) * t) for a, b in zip(fill_top, fill_bottom)) + (255,)
        ImageDraw.Draw(gradient).line([(0, y), (canvas.width, y)], fill=c)
    canvas.paste(gradient, (0, 0), glyphs)
    return canvas


def fit(font_path, text, width, start):
    size = start
    while size > 8:
        font = ImageFont.truetype(font_path, size)
        box = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=font)
        if box[2] - box[0] <= width:
            return font
        size -= 1
    return ImageFont.truetype(font_path, size)


def english(art, font_path):
    w, h = art.size
    art = frost(art, TITLE_AREA)
    tag = outlined(art.size, TAGLINE, fit(FONT_TAGLINE, TAGLINE, int(w * 0.90), 22),
                   (215, 20, 40), (190, 10, 30), [(3, (255, 255, 255))])
    title = outlined(art.size, TITLE, fit(font_path, TITLE, int(w * 0.86), 90),
                     (120, 120, 235), (40, 50, 190), [(3, (20, 24, 90)), (5, (255, 255, 255))],
                     depth=5, depth_colour=(30, 34, 110, 255))
    sub = outlined(art.size, SUBTITLE, fit(FONT_ITALIC, SUBTITLE, int(w * 0.80), 30),
                   (255, 255, 255), (235, 242, 255), [(3, (60, 90, 170))])
    y = int(TITLE_AREA[1] * h) + 20
    for piece, gap in ((tag, 2), (title, 0), (sub, 0)):
        art.paste(piece, ((w - piece.width) // 2, y), piece)
        y += piece.height + gap
    return art


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ad", type=pathlib.Path, nargs="?", default=pathlib.Path("assets/ad/bstanteiclub_ad.avif"))
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("assets/img"))
    ap.add_argument("--font", default=FONT_DEFAULT, help="a heavy sans for the English title")
    args = ap.parse_args()
    image = load(args.ad)
    for box in PAINT_OUT:
        image = paint_out(image, box)
    w, h = image.size
    art = image.crop((int(CROP[0] * w), int(CROP[1] * h), int(CROP[2] * w), int(CROP[3] * h)))
    art = art.resize((art.width * OUTPUT_HEIGHT // art.height, OUTPUT_HEIGHT), Image.LANCZOS)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    english_art = english(art.copy(), args.font)
    for name, image in (("boxart_ja.tga", art), ("boxart_en.tga", english_art), ("boxart.tga", english_art)):
        image.save(args.out_dir / name)
        print(f"{args.out_dir / name}: {image.size[0]} x {image.size[1]}")


if __name__ == "__main__":
    main()
