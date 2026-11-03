# microlink host tests

Run from the firmware repo: `.\test.ps1 host microlink`.

The suite compiles the production `ml_disco_pong.c` with transport, RNG and
NaCl test doubles. It checks that a UDP ping gets exactly one reply to its
source and no relay copy, that a relay ping gets exactly one relay reply
and no UDP copy, the 110-byte envelope and 32-byte plaintext encoding,
missing UDP addresses, encryption failure, short/failed UDP writes and
relay queue failure. Failures never fall back to another transport.

Expected: `8 Tests 0 Failures 0 Ignored`, `microlink: PASS (8 tests)`.
This tests framing and dispatch, not NaCl cryptography. The real encrypted
exchange, blocked-UDP relay traffic, rekey and direct upgrade are covered
by `.\test.ps1 tspublic` against Betty.
