#!/usr/bin/env python3
# Makes the program's icons from graphics/PreviousTurboLogo.png (needs Pillow):
#   src/gui-win/previous-winicon.ico Windows program icon
#   src/previousturbo.png            Linux desktop icon (256x256)
#   src/Previous-icon.bmp            installed with the program (512x512)
#   src/gui-sdl/icon.h               window icon built into the program
# Run from the top of the source tree: python3 graphics/make-icons.py
# The macOS icon is made from the logo when the application is built.

from PIL import Image

logo = Image.open("graphics/PreviousTurboLogo.png").convert("RGBA")

def size(n):
    return logo.resize((n, n), Image.LANCZOS)

size(256).save("src/gui-win/previous-winicon.ico",
               sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
size(256).save("src/previousturbo.png", optimize=True)
size(512).save("src/Previous-icon.bmp")

ICON = 128
pixels = size(ICON).tobytes()
with open("src/gui-sdl/icon.h", "w") as f:
    f.write("/* The window icon: %dx%d RGBA, made from graphics/PreviousTurboLogo.png\n"
            " * by graphics/make-icons.py */\n\n" % (ICON, ICON))
    f.write("#define ICON_SIZE %d\n\n" % ICON)
    f.write("static unsigned char icon_rgba[%d] = {\n" % len(pixels))
    for i in range(0, len(pixels), 16):
        f.write("\t" + ",".join("0x%02x" % b for b in pixels[i:i + 16]) + ",\n")
    f.write("};\n")
