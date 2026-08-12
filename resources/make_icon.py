"""Convert the generated icon source into a multi-resolution Windows .ico file."""
from PIL import Image, ImageDraw
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(SCRIPT_DIR, "icon_source.jpg")
DST_ICO = os.path.join(SCRIPT_DIR, "app.ico")
DST_PNG = os.path.join(SCRIPT_DIR, "app.png")

img = Image.open(SRC).convert("RGBA")
w, h = img.size
print(f"Source: {w}x{h}")

# The generated image has an "AI generated" watermark at the bottom-right
# corner. On a 1024px preview the label sits at roughly x=[780,980],
# y=[900,980]. Scale proportionally to the actual source resolution.
sx = w / 1024.0
sy = h / 1024.0
wx0 = int(760 * sx)
wy0 = int(890 * sy)
wx1 = w
wy1 = h

# Sample a pixel just above the watermark for a close background colour.
sample_x = int(900 * sx)
sample_y = int(870 * sy)
bg_sample = img.getpixel((sample_x, sample_y))
draw = ImageDraw.Draw(img)
draw.rectangle((wx0, wy0, wx1, wy1), fill=bg_sample)
print(f"Painted watermark rect ({wx0},{wy0})-({wx1},{wy1}) with {bg_sample}")

# A tiny watermark may also sit at the absolute bottom edge; trim a few px.
trim = int(6 * sy)
if trim > 0:
    img = img.crop((0, 0, w, h - trim))
    w, h = img.size

# Save a clean 256x256 PNG for Qt resource embedding.
png256 = img.resize((256, 256), Image.LANCZOS)
png256.save(DST_PNG, "PNG")
print(f"Saved clean PNG: {DST_PNG}")

# Build a multi-resolution ICO.
# Pillow's ICO writer generates each requested size from the source image;
# we pass the full-resolution image and list all desired sizes.
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
img.save(
    DST_ICO,
    format="ICO",
    sizes=sizes,
)
print(f"Saved multi-res ICO: {DST_ICO}  (sizes={sizes})")

# Verify.
verify = Image.open(DST_ICO)
print(f"Verification - sizes in ICO: {verify.info.get('sizes', 'N/A')}")
