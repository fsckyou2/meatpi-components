# filesystem: on-target test app

Self-contained (no external gear). Mounts the real `storage` LittleFS
partition from the **main partition table** (standard rev 2.1 §7). Run:

```powershell
.\test.ps1 target filesystem
```

## What is covered

Mount (format-on-first-use on a virgin partition), whole-file write/read
roundtrip, `exists`, atomic overwrite (new content visible, no `.tmp`
leftover), parent-dir auto-create, `size` + too-small-buffer reporting the
required size, `list` (file + nested dir), delete, five malformed paths
rejected, `/sd` reserved-but-unavailable (`ESP_ERR_INVALID_STATE`), validated
streaming `open`, capacity info from the real partition, the capacity of a
32 GB card (issue #982: the app runs without a card, so a link-time wrap of
`esp_vfs_fat_info` answers for `/sd` with that card's FAT volume), persistence
across unmount/remount. A failed check keeps `TEST DONE` away: the app ends
with `TEST FAILED checks=N` instead, so the stage goes red rather than pass
on "no crash".

Builds only `main` and what `filesystem` requires (`MINIMAL_BUILD`, since
2026-10-11): 820 ninja steps, about 2.5 min on the bench PC.

## Expected result: serial markers, in this order

```
MOUNT ok=1
RW body=hello-fs len=8
EXISTS file=1 missing=0
ATOMIC body=v2-content leftover=0
NESTED body=deep
SIZE n=10 small=ESP_ERR_INVALID_SIZE need=10
LIST saw_file=1 saw_dir=1
DELETE ok=1 exists=0
BADPATH rejected=5
SDPATH state=1
OPEN body=deep
INFO total_kib=5888 used_ok=1
INFO_SD err=ESP_OK total=31088689152 used=16384 expect_total=31088689152 expect_used=16384 ok=1
REMOUNT body=deep
TEST DONE
```

`INFO total_kib=5888` doubles as proof the main partition table is in use.
First mount of a virgin partition logs `mount failed, formatting...`,
expected. Last verified green: 2026-07-02 on WiCAN Pro (before the
`INFO_SD` leg).

`INFO_SD` is the volume of issue #982: a 32 GB card, FAT32, 1,897,503
clusters of 16 KiB (31,088,689,152 bytes), one cluster in use. **Reproduced
2026-10-11 on the bench unit, before the fix**: the build printed
`INFO_SD err=ESP_OK total=1023918080 used=16384 expect_total=31088689152
expect_used=16384 ok=0` and `TEST FAILED checks=1`, every other marker green
(`filesystem_info()` cast the 64-bit FATFS totals to the 32-bit `size_t`:
31,088,689,152 mod 2^32 = 1,023,918,080 bytes, the File Manager's
"976.48 MB"). **With the fix (`uint64_t` through `filesystem_info()`), the
same day: `INFO_SD err=ESP_OK total=31088689152 used=16384
expect_total=31088689152 expect_used=16384 ok=1`, `TEST DONE`, 0 E lines,
`RESULT: PASS` in 16.4 s** (`test-reports/2026-10-11_2028_target_filesystem.md`).
Both captures and the runs' output are in the public repo under
`test-reports/logs/issue982_20261011/`.
