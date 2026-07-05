# V10-N6e — Logo kernel A.L.A. PLEIN ÉCRAN répliquant l'IntroOverlay.qml
# (même Column centrée, mêmes tailles/couleurs) pour une continuité visuelle
# logo kernel -> intro app. Dessiné paysage 1280x800 puis tourné 90° CCW
# (même transform que le BMP U-Boot validé) -> 800x1280 = taille exacte du fb.
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 1280, 800
OUT = "/tmp/claude-1000/-home-michael-yocto-nxp-debix/371e3f4f-0886-44a3-954f-a2d1d2560595/scratchpad"

def font(sz):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
        try:
            return ImageFont.truetype(p, sz)
        except OSError:
            pass
    return ImageFont.load_default()

def hexc(s):
    return tuple(int(s[i:i+2], 16) for i in (1, 3, 5))

# fond : dégradé vertical 0a0d10 -> 12161a -> 080a0c (IntroOverlay)
img = Image.new("RGB", (W, H))
c0, c1, c2 = hexc("#0a0d10"), hexc("#12161a"), hexc("#080a0c")
for y in range(H):
    t = y / (H - 1)
    if t < 0.5:
        k = t / 0.5
        c = tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
    else:
        k = (t - 0.5) / 0.5
        c = tuple(int(c1[i] + (c2[i] - c1[i]) * k) for i in range(3))
    ImageDraw.Draw(img).line([(0, y), (W, y)], fill=c)

# halo ambré doux derrière le logo (opacité ~0.07)
halo = Image.new("L", (W, H), 0)
ImageDraw.Draw(halo).rounded_rectangle(
    [(W - 700) // 2, (H - 340) // 2, (W + 700) // 2, (H + 340) // 2],
    radius=170, fill=255)
halo = halo.filter(ImageFilter.GaussianBlur(60)).point(lambda v: v * 7 // 100)
img = Image.composite(Image.new("RGB", (W, H), hexc("#e5a13c")), img, halo)

d = ImageDraw.Draw(img)

def spaced_w(text, f, ls):
    return sum(d.textlength(ch, font=f) for ch in text) + ls * (len(text) - 1)

def draw_spaced(im, xy, text, f, fill, ls, alpha=255):
    x, y = xy
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    dl = ImageDraw.Draw(layer)
    for ch in text:
        dl.text((x, y), ch, font=f, fill=fill + (alpha,))
        x += dl.textlength(ch, font=f) + ls
    im.paste(Image.composite(Image.new("RGB", im.size, fill), im, layer.split()[3]),
             (0, 0), layer.split()[3])

f_top, f_ala, f_sub, f_st = font(15), font(160), font(24), font(11)

# géométrie Column (spacing 10) : hauteurs = line height Qt (ascent+descent)
ala = "A.L.A."
def lh(f):
    a, dsc = f.getmetrics()
    return a + dsc
h_top, h_ala, h_sub, h_st = lh(f_top), lh(f_ala), lh(f_sub), lh(f_st)
total = h_top + 10 + h_ala + 10 + h_sub + 10 + 26 + 10 + 3 + 10 + h_st
y = (H - total) // 2

def cx(text, f, ls):
    return (W - spaced_w(text, f, ls)) / 2

draw_spaced(img, (cx("by ELECTROSENS R&D", f_top, 6), y),
            "by ELECTROSENS R&D", f_top, hexc("#8b959d"), 6)
y += h_top + 10

ax = cx(ala, f_ala, 14)
# ombre portée (5,7) noire 65%
draw_spaced(img, (ax + 5, y + 7), ala, f_ala, (0, 0, 0), 14, alpha=166)
# corps métal
draw_spaced(img, (ax, y), ala, f_ala, hexc("#b9c2ca"), 14)
# biseau haut blanc 22%
draw_spaced(img, (ax, y - 2), ala, f_ala, (255, 255, 255), 14, alpha=56)
y += h_ala + 10

draw_spaced(img, (cx("AUDIO LIVE ASSISTANT", f_sub, 12), y),
            "AUDIO LIVE ASSISTANT", f_sub, hexc("#e5a13c"), 12)
y += h_sub + 10 + 26

# pas de barre de progression sur le logo kernel (demande utilisateur) —
# l'emplacement reste réservé pour garder la même position que l'intro app
y += 3 + 10

draw_spaced(img, (cx("DÉMARRAGE DU SYSTÈME…", f_st, 3), y),
            "DÉMARRAGE DU SYSTÈME…", f_st, hexc("#5c666e"), 3)

img.save(OUT + "/kernel_logo_preview_landscape.png")

# rotation dalle portrait (même sens que le BMP U-Boot validé)
img = img.rotate(90, expand=True)
# clut224 : quantize <=224 couleurs puis PPM ASCII P3 (pnmtologo refuse P6)
img = img.quantize(colors=224).convert("RGB")
w, h = img.size
px = img.load()
with open(OUT + "/logo_ala_fullscreen.ppm", "w") as f:
    f.write(f"P3\n{w} {h}\n255\n")
    for yy in range(h):
        f.write(" ".join(f"{px[x,yy][0]} {px[x,yy][1]} {px[x,yy][2]}"
                         for x in range(w)) + "\n")
print("PPM", w, "x", h)
