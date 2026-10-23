from pathlib import Path
import sys
from PIL import Image

if len(sys.argv) != 3:
    raise SystemExit('usage: convert_ppm_screenshot.py SOURCE TARGET')

source = Path(sys.argv[1])
target = Path(sys.argv[2])
with Image.open(source) as image:
    image.save(target, format='PNG', optimize=True)
print(target)
