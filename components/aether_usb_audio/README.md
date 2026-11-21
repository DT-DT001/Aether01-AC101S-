# USB headset and sound-card host transport

This component pins the official [Espressif usb_host_uac1.5.0](https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/readme).
Its manifest explicitly supports ESP32-S31 with the existing ESP-IDF6.1 and USB1.6.
No SDK is installed or replaced. An owned local override under
`vendor/usb_host_uac/` retains the exact official source/license with four narrow
failure-cleanup guards: NULL-safe interface cleanup, ring-buffer deletion
before interface free, and membership checking before removing a partially
constructed device from its list, plus restoring the parent reference count
when USB device close fails so a retained handle can be retried. `PROVENANCE.json` records the upstream commit and every
original/patched file hash. Managed downloads are not edited. The official class supports **UAC1.0 only**;
UAC2, proprietary codecs and24/32-bit-only devices are not advertised as supported.

The shared `aether_usb_host` installs/pumps exactly one USB core and owns the
PHY, power and physical-bus reservation. These class hooks only install the UAC
client/background handler, request stop, and report completion. They never
install a second core, pump library events, change TG29 or route USB pins.
The audio worker owns all UAC device/stream handles. Driver callbacks publish
small events or atomic cancellation flags without taking a codec/FIFO/UAC mutex.
The nonblocking teardown poll remains pending until streams, devices and the
UAC class client have actually been released; failed stop/close/uninstall retains
ownership and permits retry. No timeout frees potentially live DMA buffers.
Official driver control calls can take up to5seconds and uninstall waits for its
client handler; those operations run on this worker, outside GUI/codec locks.
Public start/stop callers wait only their supplied timeout; timed-out starts
cancel their request, and later completion cannot reactivate a cancelled stream.

Application output and capture use48kHz PCM16 stereo. Endpoints are genuinely
negotiated from descriptors: PCM16 with two-byte subframes,48000Hz preferred,
44100Hz as a supported fallback, one or two channels. A streaming rational-phase
linear converter resamples44.1/48k and duplicates mono capture or downmixes mono
playback. Converter phase persists across block boundaries. No unsupported rate
is silently substituted. Interfaces whose discrete frequency count exceeds the
configured `UAC_FREQ_NUM_MAX` (this project:32) are refused before official-driver start, including
all alternate settings, to avoid that version's truncated-array iteration path.
Unsupported interfaces produce `ESP_ERR_NOT_SUPPORTED`.
The first supported speaker and microphone are selected, possibly separate
interfaces of one headset. Extra devices require reconnect to replace a selection.

Playback/capture each have a32KiB PSRAM FIFO. Whole-block write/read contracts
require nonzero stereo-frame-aligned data, at most32768 bytes. ESP_OK means the
whole block was queued/read; timeout never acknowledges partial PCM. No caller
buffer is retained. The official UAC driver copies queued PCM into its own
ring buffer and owns the USB transfer/DMA lifetime. TX staging survives driver
backpressure; capture FIFO overflow explicitly fails/stops capture instead of
silently overwriting samples. Cached statistics distinguish input queued bytes,
bytes copied to the official TX buffer, captured bytes, software FIFO levels,
backpressure timeouts, low-water notifications and transfer/overflow errors.
These are not measurements of DAC playback or actual hardware underruns.

The existing board audio route applies the one stream/master software gain.
This transport is unity gain, with optional feature-unit unmute/0dB normalization
where supported. USB gain is independent of the local speaker70% ceiling;
unsupported hardware volume control does not masquerade as implemented control.
Capture returns normalized stereo with a mono microphone duplicated into both
slots. The application recorder saves a mono WAV using a widened L/R average,
preserving right-channel microphone content without PCM16 overflow. After mic
removal the USB input selection stays visible and unavailable until reconnect
or explicit selection of the board microphone; it never silently records a
different source. The transport has both capture and playback endpoints; the
current application serializes recording and media playback.

Run `sh tests/usb_audio/run.sh` in the existing host compiler environment.
The tests include the production worker and converter with mocked UAC/RTOS I/O,
cover real PCM flow, format negotiation, asynchronous cancellation, backpressure,
failed stream/device/client cleanup, disconnect during open/negotiation,
concurrent STOP acknowledgment and retained ownership under ASan/UBSan. A second
fixture executes extracted actual vendor open/delete bodies and the exact
interface-add failure tail with allocation/open failures and nonmember lists.
They do not qualify physical headset enumeration, isochronous bandwidth,
endpoint clock drift, audible quality or long-duration duplex operation.
No USB headset is currently available for this development task's hardware test.


The UAC owner sleeps on notifications when no stream or cleanup is pending,
including Host sessions that contain only HID/storage devices. Running audio
retains its2ms service cadence. Failed stream/device/client teardown keeps its
ownership and retries after10,20,40,80,160 and at most200ms. A new command, OFF or
physical disconnect bypasses the previous delay. A failing direction does not
slow the healthy direction's audio service; cleanup never timer-frees resources.
