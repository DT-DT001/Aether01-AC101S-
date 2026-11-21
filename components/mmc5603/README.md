# Retained MMC5603 driver

The current Aether01 board no longer has a magnetometer. The driver sources in
this directory are retained for reuse, but the project explicitly excludes the
`mmc5603` component through `EXCLUDE_COMPONENTS` in its root `CMakeLists.txt`.
The BSP does not create a device or poll its I2C address, and the firmware has no
compass page, calibration worker or compass image asset.

Reusing this driver on a future board requires deliberately restoring the build
dependency and the board/application integration.
