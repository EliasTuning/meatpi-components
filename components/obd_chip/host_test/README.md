# obd_chip: host unit tests

Pure framing/classification only (`obd_chip_parse.c`: no UART/GPIO). Run via
`.\test.ps1 host`.

## What is covered (19 tests)

| Case | What it proves |
|---|---|
| simple response | echo + payload + `>` prompt → clean extract |
| echo-off response | works after ATE0 (no echo present) |
| prompt split across chunks | every split point 1..20 reassembles identically |
| bytes after prompt not consumed | stream position hands the next transaction its bytes |
| unsolicited noise interleave | broadcast contract: noise inside the window is kept verbatim, prompt still terminates |
| **long real-car response, all splits** | meatpi's genuine 36-line `22202A` log replayed whole and at chunk sizes 1,2,3,7,16,64,127,128,4096: first/last lines intact, all 36 present |
| overflow flagged not fatal | >4 KB responses truncate with `overflow`, still terminate |
| chip error classification | `?`, UNABLE TO CONNECT, CAN ERROR vs valid data |
| monitor command classification | ATMA/ATMR/ATMT/STM(A) incl. case/space/args; ATI/0100/ATMAX are not |
| fw iterator + end marker | CR/LF stripping, empty-line skip, `FFF1` detection |
| fw version policy | (issue #1023) a chip at V2.3.22 or V2.3.24 is current (the v4.52p list), V2.3.18/V2.3.10, `?`, empty and NULL are not; the log string names both |
| eeprom guard, STSLCS (5 cases) | see `test_parse.c` / `test_stslcs.c` |
| protocol switch classification | (issue #1023, 2026-11-01) the words the request engine sends `ATM0` ahead of: `ATTP6` / `attp7` / `AT TP 8` / `ATTPA6` / `ATSP00` / `ATTPC`, `STP33` / `stp 34`, `VTP234`, a chain's first element; not `ATM0`, the resets, `ATDPN`, `ATSH`, `ATST`, the STN/vendor reads (`STPR`, `STPBR`, `STPC`, `VTPRON`), data, junk; the guard's `ATSP7` -> `ATTP7` and the classification agree |
| reset word classification | (issue #1023, 2026-11-01, Ali's ATZ-as-ATWS) a chunk that is exactly an app's `ATZ` / `ATWS` / `ATD` (any case and spacing, line ends and blanks around it) is one; a chunk with a second command behind it, `ATDPN` / `ATDP` / `ATD0` / `ATD1` / `ATWS1`, other words, junk and NULL are not; only the first `len` bytes count |

## Expected result

```
19 Tests 0 Failures 0 Ignored
OK
```
