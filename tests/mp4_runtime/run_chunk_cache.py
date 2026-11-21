"""Compile the actual two production lookup functions, with a visit counter."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]
source=(root/'components/aether_mp4_player/vendor/minimp4.h').read_text(encoding='utf-8')
begin=source.index('static int sample_to_chunk(')
end=source.index('\n#define FREE(x)',begin)
selected=source[begin:end]
loop='for (; nc < tr->chunk_count; nc++)\n    {'
assert selected.count(loop)==1
selected=selected.replace(loop,loop+'\n        ++chunk_steps;')
block_begin=selected.index('    // A chunk can contain several AAC samples.')
block_end=selected.index('    // Sequential demuxing queries',block_begin)
control=selected[:block_begin]+selected[block_end:]
for name,body,fast in [('current',selected,1),('old_control',control,0)]:
    out=root/'build/host_mp4_chunk_cache'/name
    out.mkdir(parents=True,exist_ok=True)
    (out/'chunk_cache_selected.inc').write_text(body,encoding='utf-8')
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',f'-DEXPECT_FAST={fast}',
                    '-I'+str(root/'components/aether_mp4_player'),'-I'+str(out),
                    str(root/'tests/mp4_runtime/chunk_cache_test.c'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True)
