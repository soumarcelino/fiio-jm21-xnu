# JM21 loader

Sources for the native Qualcomm handoff, USB monitor and iOS initialization.
Build host: Linux with `aarch64-linux-gnu-{gcc,ld,objcopy,readelf}`, LLVM tools,
`mkbootimg`, Python 3 and Pillow (font API with `load_default(size=...)`).

Supply these local inputs; none is included in this repository:

- `JM21_SYSTEM_TREE`: matching extracted iOS 16.5 system with `rootfs/`,
  `var.skel`, `fetch-from-ipsw.py` and the IPSW input expected by that script.
- `JM21_FIRMWARE_DIR`: your JM21 firmware directory, containing
  `filesystems/vendor/firmware/` and `filesystems/modem/image/`.
- `THEOS`: compiler tools and iPhoneOS16.5.sdk, as in the top-level README.

```sh
make mesa
export JM21_SYSTEM_TREE="$PWD/private/system"
export JM21_FIRMWARE_DIR="$PWD/private/firmware"
make boot
```

Output: `loader/out/shim-boot.img`. The build uses the included kernel and
`tools/build-storage.sh` profile. It does not install or flash anything.
Requires an already provisioned compatible root/Data layout; this snapshot does
not automate first-time OS extraction, installation or migration.

Wi-Fi credentials are never imported automatically. Do not publish generated
boot images: they can contain proprietary firmware and Apple files. Experimental
Metal remains available in source; `JM21_PUBLIC_METAL=1 make boot` opts in.
Default public build disables that incomplete path. Do not enable GPU OC.
