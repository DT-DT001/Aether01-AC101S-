# Wi-Fi owner-task regression

```sh
sh tests/wifi_runtime/run.sh
```

This executes production `aether_wifi.c` with a real pthread owner task and
thread-safe command queue, under ASan/UBSan. Only ESP-IDF radio/netif/events,
NVS and FreeRTOS endpoints are mocked. A controllable monotonic clock exercises
timeouts and retry deadlines without a long wall-clock delay.

Covered behavior:

- Default wireless OFF, nonblocking scan submission/completion, AP names,
  RSSI/security classification and password/unsupported-network validation.
- No owner task before first enable, safe no-task destruction, explicit OFF
  command rejection, first-task allocation failure/retry, and forgetting an
  already stored credential before RF has ever been enabled.
- The actual driver-init argument carries RX4/BA4/static TX4/dynamic RX12/
  cache TX8/RX management5/short management6, and both success/failure log
  paths query internal/DMA free/largest-block snapshots. Synthetic mock heap
  numbers are not printed or represented as device memory measurements.
- Association, acquired IPv4 address and NVS credential saving only after a
  successful connection. Flash-busy guards prevent writes; pending saves
  survive Wi-Fi OFF and reconnect from stored credentials when enabled.
- Forget clears RAM connection intent immediately, suppresses stale NVS
  auto-connect while erase is deferred, and retries failed NVS commits with
  backoff after media becomes idle.
- Five bounded reconnect attempts with 1/2/4/8/16-second delays, association
  and scan timeout recovery, explicit disconnect, initialization failure/retry
  and driver/netif/handler/queue cleanup.
- Explicit scan supersedes saved-network association, retry backoff or an AP
  switch, waits for the existing asynchronous disconnect and coalesces duplicate
  requests without extending the 2-second cancellation deadline. The SDK mock
  rejects scans during an actual association, reproducing the device conflict.
  Scan never enters that forbidden overlap, retains remembered credentials and
  suppresses late DHCP/disconnect/STA_START from reviving auto-connect.
- A connected background scan preserves the active link. LOST_IP still cancels
  its old scan deadline and permits ordinary reconnect; canceled scan completion
  cannot publish stale state. New connect/disconnect/forget/OFF revoke a pending
  scan, and missing/rejected cancellation or a scan-start error remains bounded
  and explicitly retryable.

- Rejected or missing disconnect completion never starts a second association.
  New AP selections during cancellation replace the target without restarting
  its deadline. A 2-second cancellation timeout closes the driver and reports
  an error; the user can explicitly re-enable and retry. Forget/disconnect also
  cancel an active scan instead of leaving a phantom scanning state.
- Stop/deinit failures preserve the real SDK ownership flags, block a second
  driver initialization, and recover once cleanup succeeds. Queued events from
  the previous radio epoch are ignored across a deterministic OFF/ON sequence.
  DHCP success requires matching current SSID/BSSID/AP info; stale LOST_IP while
  associating cannot consume a reconnect attempt.

Set `AETHER_TEST_OUTPUT` to an absolute private directory when validating concurrently.

These tests do not establish RF operation, authentication against a real AP,
DHCP/router Internet reachability, Wi-Fi/BT coexistence performance or device
stack/heap margins. Actual S31 acceptance should include an open and WPA2/WPA3
personal AP, incorrect password, AP power loss/recovery, repeated enable/disable,
forget/reboot persistence, and concurrent media playback. Check audio underruns,
LCD responsiveness, driver allocation failures and minimum internal-DMA heap;
do not infer throughput from this mock or from ESP32-S3 measurements.
