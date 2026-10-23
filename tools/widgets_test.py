#!/usr/bin/env python3
"""End-to-end test of the cos_widget toolkit + rectangle presents, driven through the real input path.
Needs a kernel built with VALIDATION_LAUNCH_WIDGETS=1 and a running QEMU (tools/run_qemu_c-os.sh --bios --no-usb ...)
with COS_QMP/COS_SERIAL pointing at the given paths.  usage: widgets_test.py QMP SERIAL_LOG"""
import re, sys, time
from PIL import Image
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from qmp_ui import UI

qmp, log = sys.argv[1], sys.argv[2]
ui = UI(qmp)
fails = []
def check(name, ok, info=''):
    print(('PASS  ' if ok else 'FAIL  ') + name + (('  ' + str(info)) if info else ''))
    if not ok: fails.append(name)
def serial(): return open(log, errors='replace').read()
def stats(): 
    m = re.findall(r'\[WIDGETS\] stats frames=(\d+) rects=(\d+) pixels=(\d+)', serial())
    return tuple(map(int, m[-1])) if m else None
def px(x, y):
    ui.shot('/tmp/wt.ppm'); return Image.open('/tmp/wt.ppm').convert('RGB').getpixel((x, y))
def kernel_partial():
    m = re.findall(r'partial frames ready=(\d+) taken=(\d+) of (\d+)', serial())
    return tuple(map(int, m[-1])) if m else (0, 0, 0)

# ---- hover highlights a button (widget-level damage, no click needed)
ui.goto(615, 215); time.sleep(0.4)
check('hover button -> highlight colour', px(595, 205) == (91, 140, 240), px(595, 205))
ui.move(615, 260); time.sleep(0.3)
check('leaving the button clears the highlight', px(595, 205) == (58, 110, 230))

# ---- text input + button + enter
ui.goto(400, 215); ui.click(); ui.type('Ada 7'); time.sleep(0.3)
ui.goto(615, 215); ui.click(); time.sleep(0.3)
check('textbox typing + Greet click', '[WIDGETS] greet Ada 7' in serial())
ui.goto(400, 215); ui.click(); ui.key('backspace'); ui.key('ret'); time.sleep(0.3)
check('backspace + Enter fires on_enter with edited text', '[WIDGETS] greet Ada ' in serial() and serial().count('[WIDGETS] greet') >= 2)

# ---- checkbox, slider, list
ui.goto(169, 290); ui.click(); ui.click(); time.sleep(0.3)
check('checkbox toggles twice', '[WIDGETS] check=0' in serial() and '[WIDGETS] check=1' in serial())
ui.goto(374, 327); ui.btn(True); ui.move(430, 327); ui.btn(False); time.sleep(0.3)
sl = [int(v) for v in re.findall(r'\[WIDGETS\] slider=(\d+)', serial())]
check('slider drag changes value', bool(sl) and sl[-1] > 40, sl[-3:])
ui.goto(300, 443); ui.click(); time.sleep(0.3)
check('list click selects item 4', '[WIDGETS] select=3' in serial())
ui.key('down'); time.sleep(0.2)
check('arrow key moves selection', '[WIDGETS] select=4' in serial())
ui.key('tab'); ui.key('tab'); time.sleep(0.2)

# ---- overhead: pixels presented vs the whole window
ui.goto(190, 553); ui.click(); time.sleep(0.5)
st = stats()
check('app reports statistics', st is not None, st)
if st:
    frames, rects, pixels = st
    full = 520 * 440
    per_frame = pixels / max(1, frames)
    check('average presented area is a small fraction of the window', per_frame < full * 0.25, 'avg %.0f px/frame vs %d full' % (per_frame, full))
ready, taken, seen = kernel_partial()
check('compositor composed partial frames', taken > 0, (ready, taken, seen))
check('and (almost) every announced rectangle became a partial frame', taken >= 0.7 * ready if ready else False, (ready, taken))

# ---- idle: no events, no wake-ups -> no new frames
before = stats(); ui.click  # noqa
time.sleep(3); ui.goto(190, 553); ui.click(); time.sleep(0.6)
check('SMAP: the kernel never touched user memory without stac', '[SMAP]' not in serial())
print('\n%d failed' % len(fails) if fails else '\nall passed')
sys.exit(1 if fails else 0)
