# NOTICE — CW V16

This product includes work derived from the projects below.

## 1. F4HWN firmware overlay platform (Apache-2.0)

`app_api.h` (the overlay App ABI header), `pack_app.py` (the FAP1 packer) and the base
`app.ld` linker script in this repository are derived from the F4HWN custom firmware:

- https://github.com/armel/uv-k1-k5v3-firmware-custom (Apache-2.0)
- https://github.com/armel/uv-k5-firmware-custom (Apache-2.0)
- https://github.com/egzumer/uv-k5-firmware-custom
- https://github.com/DualTachyon/uv-k5-firmware

The App targets the **8 KiB overlay window** fork published at
https://github.com/busybin/uv-k1-k5v3-firmware-f4hwn-8k (Apache-2.0), which adds `uid_get()`
to `app_api.h` ahead of the Labs-only block; this App does not call it, only API-level-1
services are used.

## 2. CW-F4HWN-App by lu-meiagui (no license declared)

The original functionality of this App is derived from
https://github.com/lu-meiagui/CW-F4HWN-App , which itself is based on the Beacon overlay App
of the F4HWN firmware (Apache-2.0). **That repository does not declare a license.**

Consequently:

- no license is granted or implied here for the copyrighted parts of that upstream work;
- this repository publishes **only the modifications contributed by this fork**, which are
  licensed Apache-2.0 (see [LICENSE](LICENSE));
- full attribution to the original author is preserved here and in the README;
- if you are the upstream author and would like a license added, please open an issue.

## 3. Modifications contributed by this fork (Apache-2.0)

Copyright 2026 busybin. Relative to the upstream CW app these include, non-exhaustively: the
envelope RX decoder with adaptive floor/peak thresholds and software squelch, the 7-row
message-box UI with 131-message history, prosign table, config persistence across re-install,
the double-`F` clear gesture, isolated all-T/E burst pruning, two-step `EXIT`, and the port to
the 8 KiB overlay window.

## 4. Third-party tooling

- `arm-none-eabi-gcc` (GPL with GCC Runtime Library Exception) — build only, not distributed.
