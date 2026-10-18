# fw_update host suite

Covers the PURE core only (`fw_update_version.c`, `fw_update_core.c`,
`fw_update_record.c`): no IDF glue, no network.

- the version grammar: the strict form of the manifest (`6.1.0`,
  `6.1.0-alfa.12`; leading zeros, a missing patch, the `v`, the hardware
  letter and `rc` refused) and the tolerant form of what a device reports
  (`v4.51p`, `v6.00p_alfa-06`, `v6.00p_alfa-04-2-g2a40245-dirty` as a dev
  build, `v1.22-61-gcc81591`, `v6.1.0p-alfa.1`, `alpha` as alfa, a string
  cut at 31 characters);
- the order (`6.0.0-alfa.6 < 6.0.0-beta.1 < 6.0.0 < 6.0.1 < 6.1.0-alfa.1 <
  ... < 6.1.0 < 6.10.0`, `-beta.10 > -beta.9`, a dev build equal to its base);
- channel visibility and the words;
- the manifest parse: the worked example sorted newest first, every field
  read back, the refusals (not JSON, no schema, schema 2, another device's
  manifest, no releases, no valid release), one malformed release dropped
  at a time (an http URL unless allowed, size 0, a short sha256, a version
  with a leading zero, a `requires` not below the version, a wipe without
  its labels), 70 releases cut to 64 and a 199-character summary to 140;
- the selection: up to date, the latest per channel, a dev build (equal is
  not an update, an older base is), an installed string nobody can place
  (`unknown_version`, the latest still named), the layout verdicts (none,
  wipe with its labels, lossless, serial, unknown, no MD5 in flash), the
  stepping stone and a yanked stone (`no_path`), a yanked latest skipped,
  a unit on a yanked release moved downwards;
- the URL rules and composition;
- the job record: 112 bytes, seal and validity, a flipped byte, a foreign
  version or length, the CRC-32 check vector;
- the boot decision table: nothing without a pending record, the
  bootloader's fallback against a failed boot, pending at the first boot
  and after the partition rewrite, verified after an operated run or for a
  layout-changing job, the rollback on the brake's park verdict, never for
  a layout-changing job.

Run: `.\test.ps1 host fw_update` on the Pi, or without it
`bash tools/testbench/host_gcc_wsl.sh fw_update` from WSL (gcc + ASan).
Expected: Unity `18 Tests 0 Failures 0 Ignored` (2026-10-19: + the image head parser the dongle relay reads a staged image with, `test_image.c`).
