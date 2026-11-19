"""Regenerates the small test images in this directory (needs Pillow). Run from any directory."""

import os

from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))


def path(name):
    return os.path.join(here, name)


# 4x2 RGBA: red, green, blue, white over transparent black, translucent gray, ...
pixels = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255),
          (0, 0, 0, 0), (128, 128, 128, 128), (255, 255, 0, 255), (0, 255, 255, 64)]
rgba = Image.new('RGBA', (4, 2))
rgba.putdata(pixels)
rgba.save(path('rgba.png'))
rgba.convert('RGB').save(path('rgb.png'))
rgba.convert('L').save(path('gray.png'))
rgba.convert('LA').save(path('gray-alpha.png'))
rgba.convert('P').save(path('palette.png'))
rgba.convert('I;16').save(path('gray16.png'))
rgba.convert('RGB').save(path('rgb.bmp'))
rgba.save(path('rgba.tga'))

# JPEGs with every EXIF orientation; a 3x2 image so rotations change the size
base = Image.new('RGB', (3, 2))
base.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)])
for orientation in range(1, 9):
    exif = Image.Exif()
    exif[0x0112] = orientation
    base.save(path('orient%d.jpg' % orientation), quality=95, exif=exif.tobytes())

# GIFs: a static one, and a looping animation of three frames with different delays and disposals
Image.new('P', (4, 4), 1).save(path('static.gif'))
frames = []
for color in ((255, 0, 0), (0, 255, 0), (0, 0, 255)):
    frames.append(Image.new('RGB', (4, 4), color).convert('P'))
frames[0].save(path('anim.gif'), save_all=True, append_images=frames[1:], duration=[50, 100, 20], loop=0,
               disposal=[1, 2, 3])
frames[0].save(path('anim-once.gif'), save_all=True, append_images=frames[1:], duration=40, loop=1)

with open(path('broken.png'), 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n not really a png')
with open(path('broken.gif'), 'wb') as f:
    f.write(b'GIF89a\x04\x00\x04\x00\x80\x00\x00garbage')
