from pathlib import Path
from PIL import Image

src = Path('/home/ubuntu/c-os-work/c-os/qemu_gui_boot.ppm')
dst = Path('/home/ubuntu/c-os-work/c-os/qemu_gui_boot.png')
with Image.open(src) as image:
    image.save(dst, 'PNG')
print(dst)
