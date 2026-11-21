"""Exercise the actual shim in IDF's component-group / late-import layout."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
cc = os.environ.get("CC", "cc")
out = ROOT / "build/host_h264_memory"
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as temporary:
    folder = Path(temporary)
    fixtures = {
        "heap.c": """#include "esp_heap_caps.h"
#include <assert.h>
#include <stdarg.h>
static unsigned char result;
void *heap_caps_malloc_prefer(size_t size,size_t count,...) {
 (void)size; assert(count==2); va_list args; va_start(args,count);
 uint32_t first=va_arg(args,uint32_t),second=va_arg(args,uint32_t); va_end(args);
 assert(!(first&MALLOC_CAP_INTERNAL)||(first&MALLOC_CAP_DMA));
 assert(!(second&MALLOC_CAP_INTERNAL)||(second&MALLOC_CAP_DMA)); return &result;
}
void *heap_caps_calloc_prefer(size_t n,size_t size,size_t count,...) {
 (void)n;(void)size; assert(count==2); va_list args; va_start(args,count);
 uint32_t first=va_arg(args,uint32_t),second=va_arg(args,uint32_t); va_end(args);
 assert(!(first&MALLOC_CAP_INTERNAL)||(first&MALLOC_CAP_DMA));
 assert(!(second&MALLOC_CAP_INTERNAL)||(second&MALLOC_CAP_DMA)); return &result;
}
void *esp_h264_calloc_prefer(uint32_t n,uint32_t size,uint32_t *actual,uint32_t first,uint32_t second) {
 (void)n;(void)size;(void)second; assert(first&MALLOC_CAP_DMA);*actual=320;return &result;
}
""",
        "late.c": """#include "esp_heap_caps.h"
#include "esp_h264_alloc.h"
void *aether_h264_hp_malloc_prefer(size_t,size_t,...);
void *aether_h264_hp_calloc_prefer(size_t,size_t,size_t,...);
void *aether_h264_hp_malloc(size_t);
int late_client(void) {
 unsigned internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
 unsigned external=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
 uint32_t actual=0;
 if(!aether_h264_hp_malloc_prefer(5120,2,internal,external))return 1;
 if(!aether_h264_hp_calloc_prefer(1,431,2,internal,external))return 2;
 if(!aether_h264_hp_malloc(1280))return 3;
 if(!esp_h264_calloc_prefer(1,36,&actual,internal,external))return 4;
 return actual==320?0:5;
}
""",
        "main.c": "int late_client(void); int main(void){return late_client();}\n",
    }
    options = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
               "-I" + str(ROOT / "tests/h264_memory/stubs")]
    sources = {name: folder / name for name in fixtures}
    for name, content in fixtures.items():
        sources[name].write_text(content, encoding="utf-8")
    sources["shim.c"] = ROOT / "components/aether_mp4_player/h264_hp_alloc.c"
    for name, source in sources.items():
        subprocess.run([*options, "-c", str(source), "-o", str(folder / (name + ".o"))], check=True)
    for archive, member in (("component.a", "shim.c.o"), ("heap.a", "heap.c.o"), ("libtinyh264.a", "late.c.o")):
        subprocess.run(["ar", "rcs", str(folder / archive), str(folder / member)], check=True)
    command = [cc, "-fsanitize=address,undefined", "-Wl,--gc-sections",
               "-Wl,--wrap=esp_h264_calloc_prefer", str(folder / "main.c.o"),
               "-Wl,--start-group", str(folder / "component.a"), str(folder / "heap.a"),
               "-Wl,--end-group", str(folder / "libtinyh264.a"), "-o", str(folder / "test")]
    baseline = subprocess.run(command, capture_output=True, text=True)
    assert baseline.returncode and "aether_h264_hp_malloc_prefer" in baseline.stderr, baseline.stderr
    subprocess.run([*command, "-Wl,--undefined=aether_h264_hp_malloc_prefer"], check=True)
    subprocess.run([str(folder / "test")], check=True)
    print("PASS late imported archive reproduces the unanchored link failure; one anchor pulls all four production shim entry points")
