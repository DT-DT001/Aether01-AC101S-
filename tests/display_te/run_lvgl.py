from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
test=root/"tests/display_te"
lvgl=root/"managed_components/lvgl__lvgl"
objects=sorted((root/"build/host_reader_ui").glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects:
    raise SystemExit("Build native LVGL with tests/reader_ui/build_run.py first.")
out=root/"build/host_display_te"
out.mkdir(parents=True,exist_ok=True)
for lines, diagnostic in ((40, False), (20, False), (40, True)):
    binary=out/("lvgl_te_test_"+str(lines)+("_diagnostic" if diagnostic else ""))
    compile_result=subprocess.run(["gcc","-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-pthread",
        "-ffunction-sections","-fdata-sections","-DTEST_BUFFER_LINES="+str(lines),"-DLV_CONF_INCLUDE_SIMPLE","-DLV_CONF_SUPPRESS_DEFINE_CHECK=1",
        *(["-DAETHER_VIDEO_DIAGNOSTIC=1"] if diagnostic else []),
        "-I"+str(test/"stubs"),"-I"+str(root/"tests/reader_ui"),"-I"+str(lvgl),"-I"+str(root/"main"),
        "-I"+str(root/"components/co5300_display/include"),str(root/"main/app_display_sync.c"),str(test/"lvgl_test.c"),
        *map(str,objects),"-Wl,--gc-sections","-lm","-o",str(binary)])
    if compile_result.returncode:
        raise SystemExit(compile_result.returncode)
    subprocess.run([str(binary)],check=True)
