# cmdline_manager host tests

Covers the PURE pieces of the dispatcher (`cmdline_manager_line.c`):

- the byte-stream→line assembler every transport's chunking runs
  through: whole lines, CRLF (no bogus empty line from the `\n` tail),
  lines split across chunks, several lines batched in one chunk,
  bare-newline skipping, and oversize-line discard with clean recovery;
- the verdict line a client is owed after a command: silence on success
  and on an empty line, `command failed (N)`, `Unknown command: ...` +
  `Try: help` (the longest line keeps its newline), and the
  `Internal error: <name> (0x<code>), the command did not run` line for
  an esp_console failure, which printed nothing before 2026-10-11
  (issue #980); plus safe truncation into a small buffer.

Run (on the bench Pi or any Linux with the IDF `linux` target):

    idf.py --preview set-target linux && idf.py build
    ./build/cmdline_manager_host_test.elf

Expected passing output ends with:

    13 Tests 0 Failures 0 Ignored
    OK
