#!/usr/bin/env bash
# Reproduce the installed JM21 Home profile with persistent HFSX Data in super.
# Builds only. Does not flash or format. Migration requires JM21_DATA_MIGRATE=1.
set -euo pipefail
cd "$(dirname "$0")/.."
export JM21_TOUCH_POLL_US=${JM21_TOUCH_POLL_US:-250} JM21_QUANTUM_US=500 JM21_DAS_WIDGET_CACHE=1 JM21_LOW_RAM=1
export JM21_HOME=1 JM21_MMC_HS400=1 JM21_TOUCH=1 JM21_BUTTONS=1 JM21_PERSIST=1
export JM21_FASTIO=1 JM21_TRIM=1 JM21_A610=1 JM21_MEDIA=1 JM21_BATTERY=1
export JM21_WIFI=2 ALLOW=wifi JM21_VSYNC=1 JM21_BACKGROUND_SAVE=1 JM21_METAL=${JM21_PUBLIC_METAL:-0}
export JM21_COMPACT_BOOTARGS=1
export JM21_DATA_HFS=${JM21_DATA_HFS:-1} JM21_DATA_WINDOW=${JM21_DATA_WINDOW:-super}
# Quiet profile validated over two boots; set JM21_SILENT=0 for screen diagnostics.
export JM21_SILENT=${JM21_SILENT:-1}
if [ "$JM21_SILENT" = 1 ]; then
 export JM21_OSLOG=${JM21_OSLOG-}
else
 export JM21_OSLOG=${JM21_OSLOG-'com.apple.coreduetd com.apple.analyticsd com.apple.mobileactivationd com.apple.installcoordinationd'}
fi
export JM21_EXTRA_ARGS='jm21_als_ms=300 mmc_dma=1 mmc_dma_write=1 mmc_sleep=1 wifi_station=1 jm21_input_qos=1 fb_threads=1 ca_vbl=1 jm21_ca_phase=1 jm21_ca_lead_us=4000 jm21_ca_urgent=1 jm21_present_urgent=1 metal=1 jm21_fb_direct=1'
# Optional measured-bin overclock. Omit/zero to restore stock GPU frequency.
if [ "${JM21_GPU_OC:-0}" != 0 ]; then
 [ "$JM21_GPU_OC" = 1260 ] || { echo "Unsupported JM21_GPU_OC: $JM21_GPU_OC" >&2; exit 1; }
 export JM21_EXTRA_ARGS="$JM21_EXTRA_ARGS goc=1"
fi
exec bash build.sh
