# Tools math regression

Run `bash tests/tools/run.sh` with an existing GCC/Clang host environment.

Exercises leap years, invalid dates, weekday calculation, cross-year RTC readback
timestamps, and pressure altitude/relative zero behavior. The worker regression
checks background battery sampling, media-aware preference deferral, failed-commit
retry, and preservation of pressure reference/high score from the legacy
`settings_v2` record. Retired sensor fields are ignored; subsequent writes use the
compact `settings_v3` format. A valid v3 record takes priority and malformed records
are rejected. AddressSanitizer and UndefinedBehaviorSanitizer are enabled. This
does not validate sensor accuracy, RTC I2C transactions, or NVS power-loss behavior.
