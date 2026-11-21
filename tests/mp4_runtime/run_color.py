"""Compare production I420 converters pixel-for-pixel; no host speed claim."""
from pathlib import Path
import hashlib
import os
import subprocess

root=Path(__file__).resolve().parents[2]
source_path=root/'components/aether_mp4_player/aether_mp4_player.c'
source=source_path.read_text(encoding='utf-8')
begin=source.index('static uint8_t channel(')
end=source.index('static esp_err_t decoder_new(',begin)
out=root/'build/host_mp4_color'
out.mkdir(parents=True,exist_ok=True)
(out/'production_color.inc').write_text(source[begin:end],encoding='utf-8')
print('Production converter source SHA256',hashlib.sha256(source_path.read_bytes()).hexdigest(),flush=True)
subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(out),
               str(root/'tests/mp4_runtime/color_test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
