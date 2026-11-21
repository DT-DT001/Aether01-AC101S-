# Application OTG bridge regression

Run sh tests/otg_bridge/run.sh under the existing Linux/WSL C compiler. It compiles the real main/app_otg.c with real TG29/USB public types and ASan/UBSan. Hardware, time and host scheduling are mocked; no LVGL/panel-wake symbols exist.

Coverage:

- Manual output checks software permission with detector/port OFF rather than requiring hardware CC attachment. E7 history remains diagnostic. Three explicit sessions each run beyond60s at5.134V with stale E7 states; unexpected detector enable or port-policy changes still revoke output. Each source epoch configures once. OFF installs the SINK port policy before re-enabling detection.
- Physical input absence only permits trying the separate manual admission path. GPS and the PMIC still require settled low input and verified output; manual output must be stopped before attaching a computer/charger because its disabled CC FSM cannot report attachment roles.

- External-input worker leases do not start Host or issue source/CC/BOOST/TS commands. They expose fresh voltage validity separately from readiness, block both source modes, preserve the input ADC owner across ordinary source OFF, and reject AUTO/Host cleanup/shutdown. Only explicit NOT_FOUND allows the consumer to try the manual-source admission path; bus and state errors propagate.
- Failed external acquisition may return a cleanup token. Failed release retains it and excludes source until retry succeeds. Stale tokens cannot affect a newer input owner; read faults clear the status while retaining cleanup ownership. ADC register ownership and injected read/write failures are separately covered by the production TG29 driver suite.

- Accessory manual-power leases start only from OFF or borrow a current user manual session. AUTO, teardown, concurrent admission and shutdown are rejected without waiting in the GUI. Owned release stops its session; borrowed release preserves user power, and an explicit user manual ON adopts an accessory-owned session.
- Forced OFF and worker failures immediately invalidate lease tokens. Deterministic OFF/ON publication, shutdown admission, old-token/new-session and command-contention tests verify that a stale release cannot stop restarted user power or a newer accessory lease. Admission is not measured output readiness; consumers still require a fresh output_verified snapshot before starting their peripheral.

- Lazy/default-OFF initialization performs no PMIC, ADC, PHY or input work; GUI commands only publish policy and perform atomic source cancellation.
- Automatic worker polls require fresh stable SOURCE CC/policy state. Explicit manual polls require detector/port OFF and treat E7 as diagnostic only. Initial boost-OFF is allowed; loss of expected boost, REG13 RBFET output gate, or VBUS during use returns an error for host cleanup. Input-only VBUSGOOD never substitutes for output proof.
- Trusted-ADC startup uses the PMIC extended verifier with a bounded 2000ms conversion-ready budget (not a documented ADC timing guarantee). Untrusted-ADC startup uses the separate 100ms control-settle API, retains role/permission/boost/RBFET-gate/hardware-protection checks, and never falls back after a strict verification failure.
- The ADC-trust policy is copied once from the board at initialization and remains immutable. Untrusted ADC cannot gate startup or ongoing operation on voltage validity/range, nor set output_verified/vbus_valid or copy a saturated 3FFC value into a claimed16V measurement. Raw ADC/C0 remain available; role, permission, detection, boost loss, cancellation and shutdown still stop operation.
- Both trusted and untrusted startup require fresh REG13.bit0 gate proof. Ongoing gate loss is rejected even when BOOST and the ADC still look healthy. R13 raw bits and gate state are cached with the same snapshot; failure history survives checked OFF.
- Complete failure probes atomically publish their own pre-cleanup power/R13/R50/TS/fault fields; last polling power is never combined with post-OFF idle configuration.
- Original verification failure, cleanup failure and optional diagnostic failure remain separate. Missing, cancelled or incomplete pre-cleanup probes explicitly invalidate configuration/TS rather than recycle idle or partial data.
- Failure snapshots are explicitly historical, remain readable after 2s and fifty CC retry polls, and never claim output_verified. A later successful physical verification replaces them. Live snapshots still expire after 2s.
- Raw ADC zero/fullscale and a disabled channel are distinguishable using raw ADC/C0 fields while the measured-voltage-valid flag stays false. Failed final reads retain old numeric evidence only behind invalid flags.
- Slow live TS/fault reads run at most once per second, outside the cache spinlock. Working-temperature flags are masked REG08[1:0] latches. Independent age and phase invalidation prevent rapid OFF/ON from attaching old TS/R50 to new power data.
- GUI getters perform no I2C/USB/input operations. Concurrent readers see complete cache snapshots; an additional 10,000-iteration test verifies power/TS/errors never combine across different full failure probes.
- OFF cancels verification and in-flight CC setup. Shutdown gates reject ON; a deterministic BEGIN/OFF interleaving between admission and publication is rolled back, and worker-side checks prevent regrant during shutdown.
- NES input masks and disconnect release forward without panel wake. Constructor retry, driver errors and stop timeout propagation remain covered.

The suite verifies bridge control/cache behavior. Real ADC behavior, USB-adapter wiring, output load behavior and HID enumeration still require board tests.
