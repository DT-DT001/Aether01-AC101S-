#!/bin/sh
set -eu
tests_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for suite in usb_bus usb_msc usb_host usb_audio usb_storage usb_storage_driver otg_bridge gnss_nmea gps_service reader_io frames display_metrics lvgl_memory audio recorder storage fs_lifecycle fatfs_layout fatfs_vfs_bounds s31_extram_link power user_key_ui touch text tg29 board_display reader tools arcade utilities player mjpeg_runtime nes_controls nes_runtime aac_runtime h264_memory mp4_runtime video_facade media_control bluetooth_runtime wifi_runtime; do
    printf '\nRunning %s regression\n' "$suite"
    sh "$tests_root/$suite/run.sh"
done
sh "$tests_root/pinyin/run.sh"
sh "$tests_root/text/run_pinyin_swipe.sh"
sh "$tests_root/ui_media_control/run.sh"
sh "$tests_root/../components/cst9220/test_host/run.sh"
sh "$tests_root/../components/aether_mjpeg_player/test_host/run.sh"
sh "$tests_root/../components/aether_nes/test_host/run.sh"
printf '\nAll host regression suites passed.\n'
