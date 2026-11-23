from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import importlib.util

root = Path(__file__).resolve().parents[2]
test = root / "tests/reader_ui"
out = root / "build/host_reader_ui"
out.mkdir(parents=True, exist_ok=True)
lvgl = root / "managed_components/lvgl__lvgl"
spec = importlib.util.spec_from_file_location("fatfs_unicode", root / "tests/reader/fatfs_unicode.py")
fatfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fatfs)
sources = list((lvgl / "src").rglob("*.c")) + [
    root / "main/assets/font_16.c", root / "main/assets/font_24.c",
    root / "main/apps/app_reader.c", root / "main/app_reader_layout.c",
    root / "main/apps/app_music.c", root / "main/app_lyrics.c",
    root / "main/app_reader_file.c", root / "main/app_text.c", test / "smoke.c", fatfs.source_path(root),
]
flags = ["gcc", "-std=c11", "-O0", "-g", "-Werror=implicit-function-declaration", "-ffunction-sections", "-fdata-sections",
         "-DLV_CONF_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE", "-I" + str(test / "stubs"),
         "-I" + str(root / "tests/volume_ui/stubs"),
         "-I" + str(test), "-I" + str(root / "tests/reader/stubs"), "-I" + str(root / "main"), "-I" + str(lvgl),
         "-include", str(root / "tests/reader/stubs/ff.h")]


def compile_source(source: Path) -> Path:
    try:
        name = str(source.relative_to(root)).replace("/", "_")
    except ValueError:
        name = "idf_fatfs_ffunicode.c"
    obj = out / (name + ".o")
    dependencies = [test / "lv_conf.h"]
    if lvgl not in source.parents and source.parent != root / "main/assets":
        dependencies += list((test / "stubs").glob("*.h")) + [
            root / "main/app_io.h", root / "main/app_reader_layout.h", root / "main/app_reader_file.h",
            root / "main/ui/app_scroll_list.h",
            root / "components/aether_audio_player/include/aether_audio_player.h"]
    newest = max(path.stat().st_mtime for path in [source, *dependencies])
    if obj.exists() and obj.stat().st_mtime >= newest:
        return obj
    subprocess.run(flags + ["-c", str(source), "-o", str(obj)], check=True, capture_output=True)
    return obj


try:
    with ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_source, sources))
except subprocess.CalledProcessError as error:
    print(error.stderr.decode(errors="replace"))
    raise
binary = out / "reader_ui_smoke"
subprocess.run(["gcc", "-Wl,--gc-sections", *map(str, objects), "-lm", "-o", str(binary)], check=True)
text = ("第一章 春日清晨\n\n窗外的阳光穿过树叶，在书桌上留下细碎的影子。"
                "少年翻开书，继续昨天的故事。\n\n"
                "天地玄黄，宇宙洪荒。山川与河流之间，总有新的旅程等待出发。"
                "他把书签夹好，轻轻合上窗，听见远处传来的脚步声。\n\n") * 1000
for codec,bom in [("utf-8",b"\xef\xbb\xbf"),("gbk",b""),("utf-16-le",b"\xff\xfe"),("utf-16-be",b"\xfe\xff")]:
    case = out / codec
    case.mkdir(exist_ok=True)
    book = case / "中文阅读测试.txt"
    book.write_bytes(bom + text.encode(codec))
    lyric = "[ti:海边的风]\r\n[00:38]远处灯火慢慢亮起来\r\n[00:42.00]海风轻轻穿过长街\r\n[00:48.500]我们沿着星光慢慢走"
    (case / "海边的风.lrc").write_bytes(bom + lyric.encode(codec))
    subprocess.run([str(binary), str(book), str(case)], check=True)
    print("PASS actual reader UI + project fonts + source offsets:",codec)
