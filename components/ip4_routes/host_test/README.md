# ip4_routes host test

Unity tests on the pure core (`ip4_routes_core.c`): the CIDR parser (plain,
host bits cleared, a bare address as /32, the rejects), the list parser
(separators, empty, the offending token named, too many), the formatter, and
the table's longest-prefix match, set (replace an owner's routes, refuse a
full table untouched), clear (that owner only) and ties.

Run on the bench Pi with the IDF linux target:

```
.\test.ps1 host ip4_routes
```

or by hand:

```
idf.py --preview set-target linux
idf.py build
./build/ip4_routes_host_test.elf
```

Expected output (the tail):

```
test_main.c:...:test_parse_cidr_plain:PASS
test_main.c:...:test_parse_cidr_clears_host_bits:PASS
test_main.c:...:test_parse_cidr_bare_address_and_spaces:PASS
test_main.c:...:test_parse_cidr_rejects:PASS
test_main.c:...:test_parse_list_counts_and_separators:PASS
test_main.c:...:test_parse_list_empty:PASS
test_main.c:...:test_parse_list_names_the_bad_token:PASS
test_main.c:...:test_parse_list_too_many:PASS
test_main.c:...:test_parse_list_count_only:PASS
test_main.c:...:test_format_round_trip:PASS
test_main.c:...:test_match_longest_prefix:PASS
test_main.c:...:test_match_default_route_entry:PASS
test_main.c:...:test_set_replaces_an_owners_routes:PASS
test_main.c:...:test_clear_drops_only_the_owner:PASS
test_main.c:...:test_full_table_refused_untouched:PASS
test_main.c:...:test_match_ties_take_the_first:PASS

-----------------------
16 Tests 0 Failures 0 Ignored
OK
```
