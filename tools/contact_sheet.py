#!/usr/bin/env python3
"""contact_sheet.py <dir> <out.png> [columns] [scale] -- frame dumps on one page

Takes the screens ALBW_DUMP_FRAMES wrote into a directory and lays them out in a grid, each frame's
top screen over its bottom one, labelled with the frame number: the quickest way to see where a
scripted run got to and when something went wrong. Needs Pillow (python-pillow).
"""
import os
import re
import sys

from PIL import Image, ImageDraw


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    folder, out = sys.argv[1], sys.argv[2]
    columns = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
    frames = sorted({int(m.group(1)) for name in os.listdir(folder)
                     for m in [re.match(r'frame-(\d+)-top\.ppm$', name)] if m})
    if not frames:
        print(f'no frame-*-top.ppm in {folder}', file=sys.stderr)
        return 1
    cell_w, cell_h = int(400 * scale), int(480 * scale)
    label = 14
    rows = (len(frames) + columns - 1) // columns
    sheet = Image.new('RGB', (columns * cell_w, rows * (cell_h + label)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, frame in enumerate(frames):
        cell = Image.new('RGB', (400, 480))
        cell.paste(Image.open(os.path.join(folder, f'frame-{frame:06d}-top.ppm')), (0, 0))
        bottom = os.path.join(folder, f'frame-{frame:06d}-bottom.ppm')
        if os.path.exists(bottom):
            cell.paste(Image.open(bottom), (40, 240))
        x, y = (i % columns) * cell_w, (i // columns) * (cell_h + label)
        sheet.paste(cell.resize((cell_w, cell_h), Image.NEAREST), (x, y + label))
        draw.text((x + 3, y + 1), str(frame), fill=(255, 255, 0))
    sheet.save(out)
    print(f'{len(frames)} frames')
    return 0


if __name__ == '__main__':
    sys.exit(main())
