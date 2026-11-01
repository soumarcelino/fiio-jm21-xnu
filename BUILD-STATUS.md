# Export validation

- XNU and HFS pinned revisions verified; patches applied to a fresh source tree.
- Driver integration check passed: 56 files, 12 subsystems.
- Python and shell syntax checked (33 files).
- Publication scanner passed; no binaries or Git history included.
- Original added source files compared with assembled export: only the unrelated Goodix/FocalTech driver, its firmware header and a Python cache were excluded.
- Full compilation and boot of this standalone export have not been verified.
- Toolchain/SDK, Apple user space and device firmware are external inputs; a complete boot image cannot be reproduced from this repository alone.

The scanner detects common credential formats and host paths; it is not a guarantee that arbitrary secrets can be recognized. Publish the versioned file set only.
