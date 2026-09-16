# script_engine

Berry scripting for WiCAN: a stored script (`/data/scripts/<name>.be`) or
inline source runs on the engine's own PSRAM-stack runner task against
the device bindings — UDS/OBD requests, a claimed multi-step conversation,
raw CAN, trouble codes, events — with a runtime budget and a kill switch.
The bindings ARE the scripting API; there is no privileged path (design:
`event_manager/SCRIPTING.md`).

Settings (`script_engine`, reboot-to-apply): `enabled` (default **off**),
`max_runtime_ms` (100–120000, default 10 s), `allow_reflash` (default
off — gates UDS 0x34–0x37 and `obd_transfer_file`), `exclusive` (default
**on**, 2026-09-16 — from a script's first ECU access to the end of the run
AutoPID polling and DTC scans stay off the bus: obd_gate's diagnostics
hold, independent of the UDS Tool's switch), `cli`.

## The engine describes itself (2026-09-07)

The web UI's Scripts page (editor · examples · reference) renders what
the firmware reports, so the page never drifts from the bindings:

| Route | What |
|---|---|
| `GET /api/scripts/reference` | `{language, enabled, allow_reflash, limits{src_max,file_max,out_max,sleep_max_ms,name_max,resp_max_bytes,max_runtime_ms}, groups[{id,title}], bindings[{name,sig,group,ret,doc,ex}], globals[{name,doc}], primer[{title,code,note}], errors[{match,hint}], rules{action,with,event}}` |
| `GET /api/scripts/examples` | `{examples:[{id,title,desc,needs,level,size}]}` — the gallery |
| `GET /api/scripts/examples?id=` | one example's Berry source (`text/plain`; 404 unknown) — the same handler as the list |
| `POST /api/scripts/check {src}` | compile only → `{ok}` or `{ok:false,error:"syntax_error: string:3: …"}` |

`script_engine_doc.c` holds the reference tables (one line per binding,
the readable globals, a Berry primer, hints keyed on error-message
substrings) and `script_engine_examples.c` the example programs. Both are
PURE and host-tested (`host_test/main/test_doc.c`): every binding must be
documented in a known group with a signature that names it, every example
must be a valid script name under the inline cap. The function table in
`script_engine_bind.c` is cross-checked against the docs at boot
(`se_bindings_selfcheck()` logs any binding without a reference line, or
vice versa). **Adding a binding = one line in each table.** Every example
is run on the bench against the ECU simulator; keep them runnable.

## Run surface

| Route | What |
|---|---|
| `GET /api/scripts` | `{scripts:[{name,size}], dir, busy, enabled, max_runtime_ms}` |
| `POST /api/scripts/run {src}` or `{name}` | run inline source (≤ 8 KB) or a stored script → `{ok, output}` (output ≤ 4 KB, `ERROR: <type>: <message>` + Berry's stack traceback appended on failure); 404 unknown name, 409 busy/disabled |
| `POST /api/scripts/stop` | kill switch (checked at every binding call) |

Script files are plain files: create/replace with
`POST /api/fs/upload?path=/data/scripts/<name>.be` (raw body), read with
`/api/fs/download`, remove with `DELETE /api/fs/file`. Names:
`[A-Za-z0-9_.-]`, ≤ 40 chars, no `..`.

Error messages name the line of the inline source as `string:<line>:`
(syntax errors) and in the traceback (`string:<line>: in function
\`main\``); the editor turns those into jump-to-line links.

## Rules

A rule runs a script with the `script.run {name}` action (or the sugar
`{"on":"…","script":"name"}`); the trigger event reaches the script as the
`evt_source` / `evt_name` / `evt_<field>` globals — a field exists only
when the trigger carries it (`import global` + `global.contains('evt_param')`
to test; a bare undeclared name is a compile error in Berry). Scripts
publish `script.done {value}` through `emit()` for rules to chain on.
Event-triggered runs block the dispatcher for their duration: keep them
short.

## Files

| File | Role |
|---|---|
| `script_engine.c` | VM lifecycle, runner task, run/check/kill, stored-script loader (internal-stack marshal) |
| `script_engine_bind.c` | the device bindings + the function table |
| `script_engine_doc.c` / `.h` | the reference tables + JSON builders, the engine limits (pure) |
| `script_engine_examples.c` | the built-in examples (pure) |
| `script_engine_obd.c` | the claimed-conversation core over an injected port (pure) |
| `script_engine_name.c` | script-name guard (pure) |
| `script_engine_http.c` | `/api/scripts*` |
| `script_engine_events.c` | the `script.run` action + `script.done` source |
| `script_engine_settings.c` / `_cli.c` | settings descriptor, `script run|test|stop` console command |

Host suite: `./test.ps1 host script_engine` (name guard, obd core, reflash
streamer, reference + examples).
