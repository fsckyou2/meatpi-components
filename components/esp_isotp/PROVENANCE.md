# esp_isotp: vendored component

Espressif upstream **esp_isotp v0.1.1** (isotp-c based), copied into
an earlier meatpi project and adopted here 2026-07-07 with it. Uses the
NEW `esp_driver_twai` node API. `src/isotp_config.h` is force-included
via CMake (upstream arrangement) to pin the isotp-c config.

## Local changes (this repo)

- 2026-10-19: the `Kconfig` default of `ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US`
  raised from 100000 to 1000000 (N_Bs / N_Cr per ISO 15765-2; an ECU busy
  with a flash block that answered flow control late aborted the transfer
  at 100 ms). Nothing else beyond the inherited isotp_config.h arrangement.
