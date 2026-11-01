# XNU for FiiO JM21

Experimental native XNU port for the **FiiO JM21 portable digital audio player**.
Based on XNU `8796.121.2` (iOS 16.5), with device drivers, boot loader and
user-space compatibility code.

## Device

| Component | Hardware |
| --- | --- |
| Commercial name | FiiO JM21 |
| SoC | Qualcomm Snapdragon 680, 6 nm |
| CPU | 8-core Kryo 265, up to 2.4 GHz |
| GPU | Adreno 610 |
| Memory / storage | Sold in 3 GB / 32 GB and 4 GB / 64 GB variants |
| Internal storage interface | eMMC |
| Display | 4.7-inch, 750 × 1334 touch display |
| DAC / amplifier | Dual CS43198 / dual SGM8262 |
| Audio outputs | 3.5 mm single-ended, 4.4 mm balanced |
| USB | USB Type-C, USB 2.0 |
| Expansion | microSD slot |

Product specifications: [FiiO JM21](https://www.fiio.com/jm21_parameters).
Hardware specifications do not imply driver support. Other hardware revisions
have not been validated by this export.

## Included

- `xnu/`: patches and platform code for the pinned Apple XNU release.
- `drivers/`: storage, display, GPU, touch, power, battery and Wi-Fi adaptations.
- `loader/`: boot loader, USB monitor and system initialization.
- `userland/`: compatibility adapters and supporting tools.
- `wifi/` and `tools/mesa/`: Wi-Fi and graphics compatibility code.

## Build

Linux dependencies: Git, Bash, Python 3, make, C/C++ compiler, clang/LLVM,
curl, tar, xz, flex, bison, m4, Perl, autoconf, automake and libtool.
Provide a Theos toolchain with `toolchain/linux/iphone/bin/` and
`sdks/iPhoneOS16.5.sdk/`.

```sh
export THEOS="$PWD/private/theos"
make sources
make setup
make wifi
make kernel JOBS=8
```

Output: `hakios-kernel`. These commands do not flash the device.
For boot image requirements, see [loader/README.md](loader/README.md).
SDKs, Apple user space and vendor firmware are external inputs.

## Status

- Native XNU boot and SpringBoard.
- GPU acceleration: Adreno 610 graphics path.
- Display and framebuffer.
- GT967 touchscreen.
- Wi-Fi through the ath10k compatibility layer.
- Internal eMMC storage with ADMA2 reads and writes.
- GPT partition access, APFS root and persistent HFSX Data.
- Physical buttons, battery reporting and power management.
- USB monitor and device diagnostics.

## Source verification

```sh
make audit
sha256sum -c SOURCES.sha256
```

[SOURCE.json](SOURCE.json) records upstream revisions. `SOURCES.sha256` records
the public source snapshot. Only publish versioned files; private inputs,
build outputs and dependency checkouts are ignored. Byte-identical builds
across toolchains have not been verified.
