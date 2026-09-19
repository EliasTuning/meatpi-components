# data_destinations host suite

Covers the PURE core (`data_destinations_core.c`) only — no IDF glue:

- settings-item parse/normalize: defaults (mqtt, `~/autopid`, 5 s,
  retained), scheme prepending per type, the ABRP default endpoint and
  header/query names, every rejection (missing name/url, https type with
  an `http://` URL, ABRP without a user token, auth modes without their
  credential, unknown type), disabled half-filled entries accepted,
  duplicate names refused;
- the scheduler: due-now on a fresh state, period kept on success, the
  3-failure backoff ladder (10 → 20 → 40 → 60 s cap for a 5 s period;
  8 × period / 10 min caps for long periods), success clearing it,
  offline skips counting without backoff;
- percent-encoding, URL composition (`?`/`&` joins, raw extra query,
  encoded api-key pair), dotted-IP host detection, `~/` topic expansion;
- ABRP: the snapshot→tlm map (numbers, booleans, "on"/"off", numeric
  strings, GPS names, SPEED over gps_speed, utc/car_model), the form
  body, the `APIKEY` header value, and response interpretation
  (`status` ≠ `ok` fails even on HTTP 200; non-JSON defers to HTTP).

Run: `.\test.ps1 host data_destinations` (or by hand on the Pi:
`idf.py --preview set-target linux && idf.py build && build/*.elf`).
Expected: Unity `17 Tests 0 Failures 0 Ignored`.
