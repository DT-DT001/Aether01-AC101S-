"""Compile selected actual vendor bodies; do not mirror cleanup algorithms."""
from pathlib import Path
import hashlib, json, re, sys
root = Path(__file__).resolve().parents[2]
vendor = root / "components/aether_usb_audio/vendor/usb_host_uac"
source = (vendor / "uac_host.c").read_text()
provenance = json.loads((vendor / "PROVENANCE.json").read_text())
assert hashlib.sha256((vendor / "uac_host.c").read_bytes()).hexdigest() == provenance["files"]["uac_host.c"]["vendored_sha256"]
def body(signature):
    match = re.search(re.escape(signature) + r"\s*\{", source)
    assert match, signature
    begin = source.index("{", match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]
opened = body("esp_err_t uac_host_device_open(const uac_host_device_config_t *config, uac_host_device_handle_t *uac_dev_handle)")
deleted = body("static esp_err_t _uac_host_device_delete(uac_device_t *uac_device)")
interface = body("static esp_err_t uac_host_interface_add(uac_device_t *uac_device, uint8_t iface_num, uac_iface_t **p_uac_iface)")
cleanup = interface[interface.rindex("fail:") + len("fail:"):]
# Keep exact failure statements, only supply the caller parameters and remove original closing brace.
cleanup = "static esp_err_t actual_interface_add_failure(uac_iface_t *uac_iface, esp_err_t ret) {" + cleanup
assert opened.index("vRingbufferDelete(uac_iface->ringbuf)", opened.index("fail:")) < opened.index("uac_host_interface_delete(uac_iface)", opened.index("fail:"))
assert "_uac_host_device_delete(uac_device);" in body("static esp_err_t _uac_host_device_add(uint8_t addr, usb_device_handle_t dev_hdl, const usb_config_desc_t *config_desc, uac_device_t **uac_device_handle)")
out = Path(sys.argv[1]); out.parent.mkdir(parents=True, exist_ok=True)
closed = body("esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle)")
out.write_text(deleted + "\n" + opened + "\n" + closed + "\n" + cleanup + "\n")
