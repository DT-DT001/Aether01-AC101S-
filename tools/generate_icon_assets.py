"""Resize the preserved builtin-generated originals and use LVGL's own converter.

Only regeneration needs Pillow, pypng and lz4. Firmware consumes checked-in C.
python -m pip install --target tmp/image-converter-deps pypng lz4
python tools/generate_icon_assets.py
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
assets = root / 'main/assets/icons'
manifest_path = assets / 'manifest.json'
manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
runtime = assets / 'png48'
runtime.mkdir(exist_ok=True)
env = os.environ.copy()
env['PYTHONPATH'] = str(root / 'tmp/image-converter-deps') + os.pathsep + env.get('PYTHONPATH', '')
declarations = ['#pragma once', '#include "lvgl.h"', '']
contact = Image.new('RGB', (640, 640), '#080D18')
draw = ImageDraw.Draw(contact)
for index, asset in enumerate(manifest['icons']):
    original = assets / asset['original']
    image = Image.open(original).convert('RGBA')
    low, high = image.getchannel('A').getextrema()
    if low != 0 or high < 200:
        raise ValueError(f'{original}: missing real transparency ({low},{high})')
    bounds = image.getchannel('A').point(lambda a: 255 if a > 32 else 0).getbbox()
    if not bounds:
        raise ValueError(f'{original}: empty glyph')
    image = image.crop(bounds)
    image.thumbnail((44, 44), Image.Resampling.LANCZOS)
    icon = Image.new('RGBA', (48, 48))
    icon.alpha_composite(image, ((48-image.width)//2, (48-image.height)//2))
    source = runtime / (asset['id'] + '.png')
    icon.save(source)
    name = 'icon_' + asset['id']
    subprocess.run([sys.executable, str(root / 'managed_components/lvgl__lvgl/scripts/LVGLImage.py'),
                    '--ofmt', 'C', '--cf', 'ARGB8888', '--name', name,
                    '--output', str(assets), str(source)], env=env, check=True)
    # ESP-IDF exposes the component include as lvgl.h (matching project fonts).
    converted = assets / (name + '.c')
    converted.write_text(converted.read_text(encoding='utf-8').replace(
        '#include "lvgl/lvgl.h"', '#include "lvgl.h"'), encoding='utf-8')
    declarations.append('LV_IMAGE_DECLARE(' + name + ');')
    asset['original_sha256'] = hashlib.sha256(original.read_bytes()).hexdigest()
    asset['runtime_sha256'] = hashlib.sha256((assets / (name + '.c')).read_bytes()).hexdigest()
    x, y = (index % 4) * 160, (index // 4) * 160
    contact.paste(icon.resize((96, 96), Image.Resampling.NEAREST), (x+32,y+12),
                  icon.resize((96, 96), Image.Resampling.NEAREST))
    draw.text((x+40,y+125), asset['id'], fill='#E2EBFA')
(assets / 'app_icons.h').write_text('\n'.join(declarations)+'\n', encoding='utf-8')
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
contact.save(assets / 'contact-sheet.png')
print('Generated', len(manifest['icons']), '48px ARGB8888 icons:', len(manifest['icons'])*48*48*4, 'pixel bytes')
