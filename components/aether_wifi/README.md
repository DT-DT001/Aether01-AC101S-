# Aether Wi-Fi station

The station service uses the project's existing ESP32-S31/ESP-IDF 6.1 native
`esp_wifi_*` API. Local `SOC_WIFI_SUPPORTED`/`CONFIG_ESP_WIFI_ENABLED` select the
S31 radio libraries; this does not depend on an external ESP32-S3 module. The
default IDF Wi-Fi Remote secondary-interface settings do not replace these
native calls while local Wi-Fi is enabled.

Wireless starts OFF without creating its 8KiB owner task. The first enable
request creates it; allocation failure is visible and can be retried. Status
and remembered-network availability work before first enable. Scan/connect/
disconnect return invalid-state while OFF. Forget can create the owner without
turning on RF so a stored credential is safely erased. After first creation,
the owner remains available while OFF to finish deferred persistence.
The LVGL page can enable/disable, asynchronously scan up
to 16 visible APs, inspect SSID/RSSI/security, enter a password with the native
LVGL keyboard, connect/disconnect, display IPv4 and forget a remembered network.
Open and WPA/WPA2/WPA3 personal modes are supported; enterprise, WEP, hidden
SSID entry and captive-portal login are outside this page's scope. Acquiring an
IP is a station-network result, not an Internet-reachability or SNTP check.

One command-owner task performs radio and NVS operations. UI callbacks only
enqueue commands and read locked snapshots; scans, association and flash commits
do not run on the LVGL task. Scan timeout is 15s, association timeout 20s, and
reconnection stops after five retries at 1/2/4/8/16s. Explicit disconnect/forget
cancels automatic reconnection. Driver/event errors remain visible and a toggle
cycle can retry initialization. Turning Wi-Fi OFF stops/deinitializes only the
Wi-Fi driver, preserving the global event loop and Bluetooth controller.

The component supplies a bounded native-driver initialization budget: four
static RX buffers with BA window four, four static TX buffers, 12 dynamic RX,
eight TX-cache buffers, five RX-management buffers and six short-management
buffers. This overrides the driver's large default TX reservation through the
documented `wifi_init_config_t` fields, without changing IDF or its
PSRAM-preferred allocator. Configuration and internal/DMA free/largest-block
snapshots are logged before/after each init attempt, including failures.
Actual peak memory, throughput and coexistence margins require device logs.

Passwords stay in local input/command memory and the local credential NVS blob;
they are never placed in status or logs. SDK Wi-Fi configuration storage is RAM.
The application supplies `aether_wifi_set_persistence_guard` so save/erase waits
until media is idle. A successfully connected saved request is snapshotted for
later commit; forget immediately clears connection intent and prevents reading
old credentials again while erase is pending. NVS failures retry with bounded
exponential delay. `persistence_pending` means the latest change has not yet
become durable; the page says so. Encryption at rest follows the project's NVS
configuration and is not added by this component.

Use `aether_wifi_new` once after NVS initialization, install the media-Flash guard,
and let `app_open_wifi` retrieve the singleton with `aether_wifi_get_default`.
Delete returns an error while credential persistence is pending, rather than
discarding it silently. Native owner-task regression is in `tests/wifi_runtime`;
RF coexistence, Internet services and long-run device memory margins still need
device measurement.
