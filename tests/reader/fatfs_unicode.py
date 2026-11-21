"""Locate the existing IDF Unicode table; never install a second copy/toolchain."""
from pathlib import Path
import json
import os


def source_path(root: Path) -> Path:
    base = os.environ.get("IDF_PATH")
    if not base:
        base = json.loads((root / "build/project_description.json").read_text(encoding="utf-8"))["idf_path"]
    if os.name != "nt" and len(base) > 2 and base[1] == ":":
        base = "/mnt/" + base[0].lower() + base[2:].replace("\\", "/")
    source = Path(base) / "components/fatfs/src/ffunicode.c"
    if not source.is_file():
        raise SystemExit("Set IDF_PATH to the already-installed ESP-IDF for the real CP936 conversion regression")
    return source


if __name__ == "__main__":
    print(source_path(Path(__file__).resolve().parents[2]))
