#!/usr/bin/env python3
"""Render the WiFi Drive status screen as it appears on the 172x320 panel.

The layout, colours and fonts mirror c-wifi-drive/main/ui.c. Text is drawn from the
same Montserrat bitmap fonts LVGL uses on the device (lv_font_montserrat_{20,24,48}.c,
4 bit per pixel, class kerning), so the output is what the panel shows, pixel for pixel,
apart from LVGL's anti-aliased corners on the rounded boxes.

Needs Pillow and the LVGL sources that `idf.py reconfigure` fetches into
c-wifi-drive/managed_components. The QR code comes from the `qrcode` package when it is
installed and --qr-text is given, or from a JSON module matrix (--qr-json). By default
the setup screen gets a placeholder: finder, timing and alignment patterns are real,
everything else is random, so it looks like the device's code but does not decode.

    python docs/render-screens.py            # writes docs/screens/*.png
"""
import argparse
import json
import os
import re
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONT_DIR = os.path.join(ROOT, "c-wifi-drive", "managed_components", "lvgl__lvgl", "src", "font")

W, H = 172, 320

# ---- colours from ui.c ----
PILL = {"USB": 0x1E8E3E, "WIFI": 0x1E5BD6, "EDIT": 0x1E5BD6, "WAIT": 0xC98A00,
        "EJECT": 0x505050, "NO SD": 0xC0202A}
STRIP = {"idle": 0x202020, "read": 0x40E060, "write": 0xFF8C00, "app": 0x3A86FF}
BAR_BG, BAR_FG = 0x303030, 0x3A86FF
GREY, YELLOW, WHITE = 0x9A9A9A, 0xFFD060, 0xFFFFFF


def rgb(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


# ---- LVGL bitmap font reader ----
class LvFont:
    def __init__(self, path):
        src = open(path, encoding="utf-8", errors="replace").read()

        def block(name):
            m = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", src, re.S)
            if not m:
                raise ValueError(name + " not found in " + path)
            return m.group(1)

        self.bitmap = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{1,2})", block("glyph_bitmap")))
        self.glyphs = [tuple(int(v) for v in m) for m in re.findall(
            r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
            r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", block("glyph_dsc"))]
        self.line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", src).group(1))
        self.base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", src).group(1))
        self.bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", src).group(1))
        self.kern_scale = int(re.search(r"\.kern_scale\s*=\s*(\d+)", src).group(1))
        self.left_map = [int(v) for v in re.findall(r"-?\d+", block("kern_left_class_mapping"))]
        self.right_map = [int(v) for v in re.findall(r"-?\d+", block("kern_right_class_mapping"))]
        self.kern_values = [int(v) for v in re.findall(r"-?\d+", block("kern_class_values"))]
        self.right_cnt = int(re.search(r"\.right_class_cnt\s*=\s*(\d+)", src).group(1))
        assert self.bpp == 4, "only 4 bpp fonts are handled"

    @staticmethod
    def gid(ch):
        cp = ord(ch)
        return cp - 32 + 1 if 32 <= cp < 127 else 0

    def kern(self, gl, gr):
        if gl == 0 or gr == 0 or gl >= len(self.left_map) or gr >= len(self.right_map):
            return 0
        lc, rc = self.left_map[gl], self.right_map[gr]
        if lc == 0 or rc == 0:
            return 0
        return self.kern_values[(lc - 1) * self.right_cnt + (rc - 1)]

    def advance(self, g, g_next):
        """Advance in pixels, with the same rounding LVGL applies."""
        adv = self.glyphs[g][1] + ((self.kern(g, g_next) * self.kern_scale) >> 4)
        return (adv + 8) >> 4

    def width(self, text):
        gids = [self.gid(c) for c in text]
        return sum(self.advance(g, gids[i + 1] if i + 1 < len(gids) else 0) for i, g in enumerate(gids))

    def draw(self, img, x, y, text, color):
        """Draw text with its line box top-left at (x, y), like an LVGL label."""
        px = img.load()
        base = y + self.line_height - self.base_line
        gids = [self.gid(c) for c in text]
        for i, g in enumerate(gids):
            idx, adv_w, bw, bh, ox, oy = self.glyphs[g]
            left, top = x + ox, base - bh - oy
            for row in range(bh):
                for col in range(bw):
                    n = row * bw + col
                    b = self.bitmap[idx + (n >> 1)]
                    v = (b >> 4) if (n & 1) == 0 else (b & 15)
                    if v:
                        a = v * 17 / 255.0
                        if 0 <= left + col < img.width and 0 <= top + row < img.height:
                            bg = px[left + col, top + row]
                            px[left + col, top + row] = tuple(int(round(bg[k] * (1 - a) + color[k] * a)) for k in range(3))
            x += self.advance(g, gids[i + 1] if i + 1 < len(gids) else 0)


# ---- QR code ----
def qr_placeholder(n=29, seed=7):
    """A QR-looking pattern that does not decode. Finder, separator, timing and alignment
    patterns are the real ones for a version-3 code; data and format areas are random."""
    import random
    rnd = random.Random(seed)
    m = [[0] * n for _ in range(n)]
    fixed = set()

    def finder(x0, y0):
        for y in range(7):
            for x in range(7):
                m[y0 + y][x0 + x] = 1 if (y in (0, 6) or x in (0, 6) or (2 <= x <= 4 and 2 <= y <= 4)) else 0
        for y in range(-1, 8):
            for x in range(-1, 8):
                if 0 <= x0 + x < n and 0 <= y0 + y < n:
                    fixed.add((x0 + x, y0 + y))      # finder plus its white separator

    finder(0, 0)
    finder(n - 7, 0)
    finder(0, n - 7)
    for i in range(n):
        if (6, i) not in fixed:
            m[i][6] = m[6][i] = 1 if i % 2 == 0 else 0
        fixed.add((6, i))
        fixed.add((i, 6))
    c = n - 7                                        # alignment pattern centre (22, 22) for n = 29
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            m[c + dy][c + dx] = 0 if max(abs(dx), abs(dy)) == 1 else 1
            fixed.add((c + dx, c + dy))
    for y in range(n):
        for x in range(n):
            if (x, y) not in fixed:
                m[y][x] = 1 if rnd.random() < 0.5 else 0
    return m


def qr_matrix(text, qr_json):
    if qr_json:
        data = json.load(open(qr_json))
        return data["modules"] if isinstance(data, dict) else data
    if not text:
        return qr_placeholder()
    try:
        import qrcode
    except ImportError:
        sys.exit("no 'qrcode' package: install it, or pass --qr-json, or drop --qr-text for the placeholder")
    q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=0)
    q.add_data(text)
    q.make(fit=True)
    return q.get_matrix()


# ---- the screen ----
def render(state, fonts, qr=None, ip="192.168.1.42", host="wifidrive.local",
           space="12.3 GB free", pct=61, strip=None):
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    f48, f24, f20 = fonts

    word = "USB" if state in ("SETUP", "USB") else state
    d.rounded_rectangle((8, 12, 8 + 156 - 1, 12 + 104 - 1), radius=20, fill=rgb(PILL[word]))
    tw = f48.width(word)
    f48.draw(img, 8 + (156 - tw) // 2, 12 + (104 - f48.line_height) // 2, word, rgb(WHITE))

    if strip is None:
        strip = "app" if state in ("WIFI", "EDIT") else "idle"
    d.rounded_rectangle((8, 126, 8 + 156 - 1, 126 + 18 - 1), radius=7, fill=rgb(STRIP[strip]))

    if state == "SETUP":
        # lv_qrcode: 128 px canvas, integer scale, centred; white border 4 px inside the bounds
        d.rectangle((22, 145, 22 + 127, 145 + 127), fill=(255, 255, 255))
        n = len(qr)
        scale = 128 // n
        margin = (128 - n * scale) // 2
        for yy in range(n):
            for xx in range(n):
                if qr[yy][xx]:
                    x0, y0 = 22 + margin + xx * scale, 145 + margin + yy * scale
                    d.rectangle((x0, y0, x0 + scale - 1, y0 + scale - 1), fill=(0, 0, 0))
        f20.draw(img, (W - f20.width(host)) // 2, 275, host, rgb(YELLOW))
        f20.draw(img, (W - f20.width("192.168.4.1")) // 2, 297, "192.168.4.1", rgb(WHITE))
        return img

    f24.draw(img, (W - f24.width(ip)) // 2, 162, ip, rgb(WHITE))
    f20.draw(img, (W - f20.width(host)) // 2, 196, host, rgb(GREY))
    f24.draw(img, (W - f24.width(space)) // 2, 244, space, rgb(WHITE))
    d.rounded_rectangle((8, 296, 8 + 156 - 1, 296 + 14 - 1), radius=5, fill=rgb(BAR_BG))
    iw = 156 * pct // 100
    if iw > 0:
        d.rounded_rectangle((8, 296, 8 + max(iw, 10) - 1, 296 + 14 - 1), radius=5, fill=rgb(BAR_FG))
    return img


STATES = {
    # name: (state, extra kwargs)
    "usb":       ("USB", {}),
    "usb-read":  ("USB", {"strip": "read"}),
    "usb-write": ("USB", {"strip": "write"}),
    "wifi":      ("WIFI", {}),
    "edit":      ("EDIT", {}),
    "wait":      ("WAIT", {}),
    "eject":     ("EJECT", {}),
    "no-sd":     ("NO SD", {"space": "no card", "pct": 0}),
    "setup":     ("SETUP", {"host": "WiFi-Drive-1A2B"}),
}
ROW = ["usb", "wifi", "edit", "wait", "eject", "no-sd", "setup"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "screens"))
    ap.add_argument("--fonts", default=FONT_DIR)
    ap.add_argument("--scale", type=int, default=2, help="integer upscale of the single images")
    ap.add_argument("--qr-json", help="JSON file with a QR module matrix to draw instead of the placeholder")
    ap.add_argument("--qr-text", help="encode this text as a real QR code (needs the 'qrcode' package)")
    a = ap.parse_args()

    fonts = tuple(LvFont(os.path.join(a.fonts, "lv_font_montserrat_%d.c" % s)) for s in (48, 24, 20))
    qr = qr_matrix(a.qr_text, a.qr_json)
    os.makedirs(a.out, exist_ok=True)

    panels = {}
    for name, (state, kw) in STATES.items():
        img = render(state, fonts, qr=qr, **kw)
        panels[name] = img
        big = img.resize((W * a.scale, H * a.scale), Image.NEAREST)
        big.save(os.path.join(a.out, name + ".png"), optimize=True)
        print("wrote", name + ".png")

    gap = 16
    sheet = Image.new("RGBA", ((W + 2) * len(ROW) + gap * (len(ROW) - 1), H + 2), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sheet)
    for i, name in enumerate(ROW):
        x = i * (W + 2 + gap)
        sd.rectangle((x, 0, x + W + 1, H + 1), outline=(58, 58, 58, 255))
        sheet.paste(panels[name], (x + 1, 1))
    sheet = sheet.resize((sheet.width * a.scale, sheet.height * a.scale), Image.NEAREST)
    sheet.save(os.path.join(a.out, "all-states.png"), optimize=True)
    print("wrote all-states.png")


if __name__ == "__main__":
    main()
