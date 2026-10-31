# MeatPi Firmware Component Coding Standard: rev 2.8

Applies to all firmware components across MeatPi products (WiCAN, ESPNetLink, the ECU simulator, and future devices): the WiCAN Pro v6 tree is the reference implementation the examples cite. Written for both human and AI contributors. Keep it boring and consistent.

**Targets:** ESP-IDF **v6.0.2 (pinned, the version the project builds and tests with; was "v5.3+" before reconciliation 2026-07-03)**, ESP32-S3 (PSRAM present). On chips without PSRAM the §2 PSRAM rules degrade to internal-RAM-only; everything else applies unchanged. Verified against v6 in this tree: littlefs (managed `joltwallet/littlefs`), `esp_http_server`, the `esp_log` vprintf hook (log_manager), `esp_driver_uart`/`esp_driver_gpio`. v6 gotcha: the bundled `json` component was removed, depend on the **`espressif/cjson` managed component** (public `REQUIRES` only when a public header exposes `cJSON`). The canonical minimal config is the repo-root **`sdkconfig.defaults`** (extracted with `idf.py save-defconfig`); test apps inherit from it per §7.

**Rev 2 changes:** settings are **reboot-to-apply**, `on_apply` runs once at boot, never at runtime (§4); boot-failure fallback to defaults defined (§4); defaults come from the JSON Schema, descriptor override is optional (§4); storage guarantees made normative (§4); `on_migrate` hook added (§4, §5); PUT pinned to full-replace (§6); `_deinit` forbidden (§3); Kconfig names pinned (§2); flash-write/PSRAM-stack corollary added (§2); Debug Log Manager added (§9); logging conventions added (§10); §8 checklist expanded.

**Rev 2.1 changes:** on-device (pytest) test apps must build against the **main firmware's partition table and sdkconfig**, no private forks; needing a config or partition change means changing the main firmware's config first (§7).

**Rev 2.2 changes:** schemas may be authored as a **C field table** (`settings_field_t` + `SETTINGS_*` row macros); the Settings Manager generates the JSON Schema at registration. JSON Schema remains the wire format and source of truth (§4.1, §5).

**Rev 2.3 changes:** **no panic paths in production code**, `ESP_ERROR_CHECK` (and anything else that aborts on an `esp_err_t`) is forbidden in components AND in `main`; boot must always complete (§3). HTTP routes must be documented in `components/HTTP_API.md` in the same change that adds them (§6, §8).

**Rev 2.4 changes:** console commands are a first-class component surface (§6b): a component owning CLI commands ships `<comp>_cli.c` + `<comp>_register_cli()` and calls it **itself** from its settings `on_apply`, gated by a `cli` bool setting (default true, reboot-to-apply): main wires nothing. Components with no other settings carry a minimal `{cli}` descriptor.

**Rev 2.5 changes:** the **field table is the only schema authoring format** for components, hand-written JSON Schema strings are retired (the table now expresses bounded arrays of objects via `SETTINGS_ARRAY`/`SETTINGS_ARRAY_ANY`, with JSON defaults written unescaped through `SETTINGS_JSON`). All settings code lives in a dedicated **`<comp>_settings.c`** (field table, descriptor, hooks, boot-applied config + getters), registered via `<prefix>_settings_register()` from `<comp>_init()` (§4.1).

**Rev 2.6 changes (2026-07-19, after the every-boot-flash-rewrite bug):** flash-write discipline is normative, boot-path and periodic writes MUST be change-guarded (§11); bounded registries expose occupancy and the health surface (fault codes, flash counters, log counters) is a first-class contract (§12); `ESP_LOGE` is load-bearing: a clean boot and every positive-path test log ZERO errors, enforced by bench assertions and per-test error budgets (§7, §10); mode-gated chip-register writes require read-back verification (§3).

**Rev 2.8 changes (2026-09-21, BLE as an app platform):** the BLE GATT surface is a documented contract like HTTP routes and CLI commands (§6c): `ble_manager/BLE_API.md` owns the services/characteristics table and the stream-channel registry; a component that registers a BLE channel or changes the bytes on one updates that table and its own protocol document in the same change, with byte-exact examples; characteristics are only ever appended within a service so bonded clients' cached handles stay valid.

## 1. Language & Style

- **C (C11)**, ESP-IDF. C++ only where a dependency forces it.
- **Allman brace style**, no exceptions:

```c
esp_err_t comp_do_thing(int arg)
{
    if (arg < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}
```

- 4-space indent, no tabs. Lines ≤ 100 cols.
- Naming: `snake_case` for functions/vars, `UPPER_SNAKE` for macros/constants, component prefix on all public symbols (e.g. `wican_can_*`).
- Header guards: `#pragma once`. Include order in `.c` files: own header first, then C stdlib, then IDF/FreeRTOS, then other components, then private headers. Own-header-first catches missing includes in the public header.
- One component = one directory. Public API in `include/<component>.h`; everything else is `static` or private.
- **≤ 700 lines per file.** Over that, split by responsibility (e.g. `comp.c`, `comp_settings.c`, `comp_api.c`): never split mid-logic just to hit the number.

## 2. Memory

- **Default to PSRAM for everything**: task stacks, buffers, queues, large structs, component state. Use internal RAM only when a hard constraint below forces it.
- **Prefer static allocation in PSRAM** over dynamic. Place static/global data in PSRAM `.bss` with `EXT_RAM_BSS_ATTR` (or `EXT_RAM_NOINIT_ATTR` for noinit). Allocate task stacks as static buffers in PSRAM and create with `xTaskCreateStatic`:

```c
static StaticTask_t s_tcb;                            // TCB: internal RAM
static StackType_t  s_stack[8192] EXT_RAM_BSS_ATTR;   // stack: PSRAM
xTaskCreateStatic(comp_task, "comp", 8192, NULL, 5, s_stack, &s_tcb);
```

  Required Kconfig (ESP32-S3): `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` (for `EXT_RAM_BSS_ATTR`), `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` (for PSRAM task stacks; IDF 6.1 renamed it from `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`, which survives in `sdkconfig` only in the deprecated block: a Kconfig `depends on` the old name evaluates false silently, found 2026-10-31), and `CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y` if `EXT_RAM_NOINIT_ATTR` is used.

- **Must stay in internal RAM**: these are the only acceptable reasons to leave PSRAM:
  - **DMA buffers** (TWAI/CAN, SPI, I2S, USB, …). Static: `DMA_ATTR`. Dynamic: `MALLOC_CAP_DMA`.
  - **Anything reached with the cache disabled**: data/code touched by an IRAM ISR (`ESP_INTR_FLAG_IRAM`), or accessed during flash/OTA/LittleFS writes. PSRAM is unreachable when cache is off, so this is a crash, not a perf issue.
  - **A task whose path hits the above** (IRAM ISR handler, cache-disabling work) → its stack stays internal too. **Corollary: a task running on a PSRAM stack must never touch the flash directly**, reads included (a LittleFS or NVS read, `esp_partition_read`, `esp_partition_mmap` and the OTA state reads all switch the cache off; the beta.1 crash of 2026-10-21 was an otadata read from the webhook poster), hand the work to a task with an internal stack. Since 2026-10-31 that task is the shared `flash_jobs` worker: `flash_jobs_run(fn, arg)` runs the job in line on an internal stack and on the worker otherwise (the Settings Manager, the filesystem, the NVS records and the OTA session route through it, which is what lets the HTTP server task live on a PSRAM stack, §4). IDF asserts the rule (`cache_utils.c`, `esp_task_stack_is_sane_cache_disabled`), so a missed path is an immediate abort with a backtrace, never a silent fault.
  - **Hot, latency-sensitive paths** where PSRAM access cost is measured and matters.
- When you use internal RAM, say why in a one-word comment: `// internal: DMA`.
- **PSRAM-stack overflow is SILENT** (found the hard way 2026-07-22, autopid DTC
  job task): an overflowing internal-RAM stack panics almost immediately, but a
  PSRAM stack just scribbles into the neighbouring `.ext_ram.bss`, the task
  "works" while returning deterministically corrupted data, and which bytes
  survive shifts with codegen (an unrelated edit can make the symptom vanish).
  Discipline: `StackType_t` is BYTES on xtensa, budget the task's largest
  frames explicitly (one >1 KB local array is a red flag; the autopid response
  assembler's line table alone was ~5.6 KB until it moved to a shared
  lock-guarded PSRAM static), verify with `uxTaskGetStackHighWaterMark`, and
  treat <1 KB headroom as a bug. Debug trick: temporarily moving a suspect
  PSRAM stack to internal RAM converts silent corruption into an immediate,
  locatable panic. TOOLING (2026-07-22): `-fstack-usage` is permanently on,
  `tools/stack_audit.py` cross-references compiler frame sizes against every
  task stack (both placements), and `.\test.ps1 stackaudit` adds the runtime
  half (live watermarks incl. ephemeral job tasks + heap floors); the runtime
  audit also rides the `live` suite. Ephemeral one-shot tasks must log
  `uxTaskGetStackHighWaterMark(NULL)` just before `vTaskDelete(NULL)` so the
  audit can see them (pattern: autopid's dtc job / std scan tasks).

**PSRAM `.noinit` (reboot-surviving state).** `EXT_RAM_NOINIT_ATTR` data survives
warm resets (esp_restart / panic / watchdog) but is random after a power cycle,
so it MUST sit behind a magic/version/CRC envelope with `esp_cache_msync`
write-back after every mutation (see `restart_tracker`: the reference
implementation). Two hardware gotchas, both found the hard way on WiCAN Pro:

- **`CONFIG_SPIRAM_MEMTEST` must be off**: the boot memtest writes ALL of
  PSRAM and wipes `.noinit` (disabled in the main sdkconfig 2026-07-03).
- **The first 64 bytes of `.ext_ram_noinit` are clobbered every boot** by the
  ESP32-S3 MSPI PSRAM timing tuning (test pattern at PSRAM physical address 0,
  `MSPI_TIMING_TEST_DATA_LEN`). Every noinit structure MUST therefore begin
  with a sacrificial `uint8_t mspi_tuning_guard[64]` excluded from its CRC,
  and multi-part data (header + buffer) must be ONE object so the buffer can't
  independently link at address 0.

## 3. Component API

- Every component exposes a single public header declaring its API for main firmware and other components.
- Lifecycle is explicit and uniform:

```c
esp_err_t <comp>_init(void);     // register, allocate, no side effects on bus
esp_err_t <comp>_start(void);    // begin operation
esp_err_t <comp>_stop(void);
```

- **No `_deinit`.** Components are init-once for the life of the firmware; `stop()` is the only teardown. `_deinit`/`_free` functions add untested paths and invite use-after-free. Exception: host test builds may provide a test-only reset so the Unity harness can re-init between cases, keep it out of the public header, guarded by the host-test build flag.
- Return `esp_err_t`. No silent failures, no `abort()` in library code.
- **No panic paths anywhere in production code (rev 2.3).** `ESP_ERROR_CHECK`,
  `ESP_ERROR_CHECK_WITHOUT_ABORT`-then-`abort()`, `assert()` on an `esp_err_t`,
  or any construct that panics on a recoverable failure is forbidden in
  components **and in `main`**. A deterministic init failure under
  `ESP_ERROR_CHECK` is a **permanent boot loop**: the one thing a field
  device must never do. Instead: log at ERROR and continue degraded. The
  composition root uses log-and-continue helpers (`main_boot_init` /
  `main_boot_start` in `main/main_boot.c`); a component whose init failed simply stays
  unconfigured and its `_start()` refuses per §4.3 step 5, while the rest of
  the device keeps booting (serial console, and usually the AP + OTA recovery
  path, stay alive). `ESP_ERROR_CHECK` remains acceptable in **test apps and
  host tests only**: there, failing loudly is the point. (`assert()` on
  programmer invariants that cannot depend on hardware/input state (never on
  return codes) is tolerated but discouraged.)
- A component left unconfigured at boot (§4, defaults also rejected) must refuse `_start()` with `ESP_ERR_INVALID_STATE` rather than run half-configured.
- No cross-component access to internals: talk through public APIs only.
- **Chip bring-up: an I2C/SPI ACK is not a write (rev 2.6).** Chips with mode-gated register maps (standby vs active) ACK writes they then discard, and need settle time after a mode change before config registers accept values. Critical config writes (output enables, mode bits) get a **read-back verify with a bounded retry**, and steady-state code self-heals them where cheap. Lesson: the AW2023's channel-enable write, issued immediately after CHIPEN over the fast `i2c_master` driver, was silently dropped, the LED had never lit on v6 while every layer reported success (§11).

**Dependencies.** Components may depend on other components:

- Declare every dependency in `CMakeLists.txt`. Use `PRIV_REQUIRES` when only the `.c` files use it; use `REQUIRES` only when the public header exposes the dependency's types. Favour `PRIV_REQUIRES` to keep the graph shallow.
- Depend on a component's **public header only**, never its internals.
- **No circular dependencies.** If two components need each other, that's a design smell: extract the shared part into a lower component, or invert it with registration/callbacks (the way components register with the Settings Manager and Log Manager instead of those depending on them).
- **Dependencies flow one direction:** core (log manager, settings manager, storage, HAL) ← services ← app. A lower layer never depends on a higher one.
- A component assumes its dependencies are already `*_init()`'d; it does **not** init or start them itself. `main` composes the system and brings components up in dependency order: `log_manager` first (§9.6), then `settings_manager`, then everything else.
- Keep pure logic free of heavy deps so it stays host-testable (§7); inject or stub the dependency where practical.

**Every component ships a `README.md`** at its root (`components/<comp>/README.md`) containing:

- **Summary**: one short paragraph: what it does, where it sits.
- **API**: the public functions/types from the header, one line each. Keep in sync when the header changes.
- **Dependencies**, which components it requires, and any required init order.
- **Settings**: schema keys and defaults, if it has any, plus the current schema `version`.
- **Memory footprint**: internal RAM and PSRAM, split into static (`.bss`/`.data`), task stacks (count × size, and where), and peak heap. Mark each number **estimated** or **measured**.

Measure, don't guess where you can: `idf.py size-components` for static, `uxTaskGetStackHighWaterMark()` for real stack use, and `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` / `MALLOC_CAP_SPIRAM` deltas around init for heap. Estimates are fine to start: replace with measured before release.

## 4. Settings

Don't give each component raw `store()` / `load()` callbacks. That scatters persistence logic, duplicates storage code, and makes schema validation inconsistent.

**Instead: the Settings Manager owns persistence. Components register a descriptor. Settings apply only at boot: changing settings means persist, then reboot.**

### 4.1 Descriptor and API

```c
typedef struct
{
    const char *name;           // component id; settings key + on-disk filename
    uint32_t    version;        // schema version, for migrations

    const settings_field_t *fields; // field-table schema (rev 2.5: the ONLY
    size_t field_count;             // authoring format for components)

    const char *defaults_json;  // OPTIONAL whole-object override of the
                                // schema-derived defaults; NULL for most components

    // Called ONCE, at boot, in the Settings Manager's init context,
    // before any component's _start(). Never called at runtime.
    // On failure the manager retries once with defaults (§4.3).
    esp_err_t (*on_apply)(const cJSON *settings);

    // Optional: reject bad input before commit. err buffer for UI/API.
    // Called at set() time and against stored data at boot.
    esp_err_t (*on_validate)(const cJSON *settings, char *err, size_t err_len);

    // Optional: migrate stored settings from an older schema version.
    // Mutates `settings` in place. Called ONCE with the stored version;
    // the component handles ANY from_version it has ever shipped.
    esp_err_t (*on_migrate)(uint32_t from_version, cJSON *settings);
} settings_descriptor_t;

esp_err_t settings_manager_register(const settings_descriptor_t *desc);
esp_err_t settings_manager_get(const char *name, cJSON **out);      // caller frees
esp_err_t settings_manager_set(const char *name, const cJSON *in);  // validate + persist ONLY
```

**Schema authoring: the field table, always (rev 2.5).** Components author
their schema as a `settings_field_t` table: one row per key, `SETTINGS_STR` /
`SETTINGS_STR_LEN` / `SETTINGS_STR_ENUM` / `SETTINGS_STR_FILE` /
`SETTINGS_INT` / `SETTINGS_BOOL` macros (`_REQ` variants mark a key required:
meaningful inside array items). The manager generates the equivalent JSON
Schema once at registration and rejects malformed tables
(`ESP_ERR_INVALID_ARG`) at the call site. Validation, defaults, and
`GET /settings/<name>/schema` all run on the generated JSON Schema: the wire
format is unchanged. The descriptor's raw `schema` string member still exists
but is reserved for the Settings Manager's own tests; **components must not
hand-write JSON Schema strings.**

Bounded **arrays of objects** are expressed in the table too: `SETTINGS_ARRAY`
takes a nested item-field table (scalar rows only, one nesting level, matching
the validator; `maxItems` is required, 1..16), `SETTINGS_ARRAY_ANY` takes
free-form object items (pair it with an `on_validate` that enforces the shape).
Array defaults are JSON array literals written **unescaped** through
`SETTINGS_JSON(...)`: the preprocessor stringizes it, so no `\"` soup. One
gotcha: every JSON string inside `SETTINGS_JSON` must stay a single C token
(adjacent-literal concatenation happens after stringization and corrupts the
JSON).

```c
static const settings_field_t SERVER_ITEMS[] =
{
    SETTINGS_STR_REQ     ("name",  1, 15, ""),
    SETTINGS_STR_ENUM_REQ("proto", "tcp,udp", "tcp"),
    SETTINGS_INT_REQ     ("port",  1, 65535, 3333),
    SETTINGS_BOOL        ("enabled", false),
};

static const settings_field_t FIELDS[] =
{
    SETTINGS_STR_ENUM("mode",       "off,sta,ap,apsta", "apsta"),
    SETTINGS_STR_LEN ("ap_password", 8, 64, "12345678"),
    SETTINGS_INT     ("ap_channel",  1, 13, 6),
    SETTINGS_BOOL    ("ap_auto_disable", false),
    SETTINGS_ARRAY   ("servers", 4, SERVER_ITEMS,
        SETTINGS_JSON([
            {"name":"obd0","proto":"tcp","port":35000,"enabled":true}
        ])),
};
```

**File placement: `<comp>_settings.c` (rev 2.5).** Every component with
settings keeps ALL of its settings code in a dedicated `<comp>_settings.c`:
the field table(s), the descriptor, `on_apply` / `on_validate` / `on_migrate`,
and the boot-applied config those hooks fill. The file exposes
`<prefix>_settings_register(void)` (called from `<comp>_init()`) plus
`const`-returning getters for the applied config (the
`wm_settings_config()` / `wm_settings_is_configured()` pattern:
wifi_manager and socket_manager are the reference implementations). State the
component mutates at runtime stays in the engine files; the settings file's
state is written only by its hooks. This makes every component's keys and
defaults findable in one predictable place.

### 4.2 Apply model: reboot-to-apply

- `settings_manager_set` = schema validate → `on_validate` → persist. **It never calls `on_apply`.** There is no live-reconfiguration path; components contain zero reload code.
- `on_apply` runs **once per component, at boot, single-threaded**, in the Settings Manager's init context, before any `_start()`. No threading contract is needed: it cannot race the component's own tasks because they don't exist yet.
- User flow: the UI collects edits locally; **Submit changes** sends them to the device; the device validates and persists each component's object, responds to the UI, then reboots itself. The transport must delay `esp_restart()` (≈1 s) so the HTTP response flushes, otherwise the UI sees a dropped connection instead of confirmation.
- **No-op submits skip the reboot.** If every submitted object is byte-identical to what's persisted (§4.4 dedup), nothing was written and the device must not reboot.
- Ephemeral runtime knobs (e.g. live log level, §9.4) are **not settings**. A component may expose its own runtime API for them; persisted settings still only apply at boot, and runtime knobs reset to persisted defaults on reboot.

### 4.3 Boot sequence and failure handling

At boot, for each registered component, the manager:

1. Loads the stored object. Missing or corrupt (CRC fail) → build defaults (schema `default` keywords, or `defaults_json` if set), persist them, log a WARNING.
2. If stored `version` < descriptor `version` → call `on_migrate`, re-validate the result against the schema, persist. Migration absent or failed → defaults, log an ERROR.
3. Validate against the schema and `on_validate`. Invalid → defaults, log a WARNING.
4. Call `on_apply`. **If it fails:** persist defaults, call `on_apply(defaults)` once more, and log an ERROR through the Log Manager. This covers data that is schema-valid but unachievable on the hardware (e.g. a bitrate this crystal can't hit).
5. If **defaults also fail apply**, that's a component bug, not bad data: log at ERROR, leave the component unconfigured, and continue boot. The component's `_start()` must then refuse with `ESP_ERR_INVALID_STATE`. One broken component must never brick the device; retry loops and reboot-on-failure are forbidden here: they boot-loop.
6. `GET /settings/<name>` responses include `"degraded": true` when step 4 or 5 occurred, so the UI can surface it after the reboot that triggered it.

### 4.4 Storage guarantees (normative)

The Settings Manager is the only code that touches settings storage. These behaviors are requirements: do not "simplify" them away:

- **Owns the filesystem.** It decides the FS type (LittleFS today), mounts it, and owns the layout. No other component mounts or touches the FS for settings, so the FS can change without touching any component.
- **Dedicated partition.** Settings live on their own partition, independent of the general `filesystem` component, so a corrupted user filesystem can never take down settings.
- **Atomic writes**: write to a temp file, sync, rename over the target. A power cut mid-write must leave either the old object or the new one: never a torn file.
- **Integrity**: every stored object carries a CRC-32 (polynomial `0xEDB88320`); a mismatch at load is treated as missing (§4.3 step 1).
- **Write deduplication**: `set` with content identical to what's persisted returns `ESP_OK` without writing (the `persisted_clean` guard). This protects flash from redundant wear and is what makes the no-op-submit reboot skip (§4.2) work.
- The manager's **storage runs on an internal-RAM stack whoever the caller is** (§2 corollary: flash access from a PSRAM stack is forbidden): every read and write of a settings file goes through `flash_jobs_run()`, in line for an internal-stack caller and on the `flash_jobs` worker for a PSRAM-stack caller (the HTTP server task since 2026-10-31).

## 5. Settings format

- Stored as **JSON**, one object per component keyed by `name`.
- The **JSON Schema is the single source of truth** for shape, types, ranges, and defaults (via `default` keywords), generated from the component's field table at registration (§4.1). `defaults_json` exists only for the rare component that needs a whole-object override; when present it replaces the schema-derived defaults wholesale.
- Bump `version` on any breaking schema change and extend `on_migrate` to handle every `from_version` the component has ever shipped. Non-breaking additions (a new optional key with a schema `default`) don't need a bump: validation fills the gap from the schema.

## 6. Remote settings API

- HTTP / TCP / WebSocket access is **optional per component** but goes **through the Settings Manager**, not bespoke per-component storage code.
- The manager exposes generic endpoints; the transport layer is shared:
  - `GET  /settings/<name>` → current JSON (plus `"degraded": true` per §4.3)
  - `PUT  /settings/<name>` → validate + persist (returns the `on_validate` error message on reject)
  - `GET  /settings/<name>/schema` → schema, so UIs/tools self-describe
- **PUT is full-object replace.** The client sends the complete object; the manager validates it against the whole schema (including `required`). No merge-patch: the UI already holds the full object from `GET`, and full-replace means validation always sees a complete document. Partial updates failing schema validation is the correct outcome.
- After the last successful PUT of a submit batch, the transport responds, then reboots the device (§4.2). If nothing changed, it responds without rebooting.
- A component that needs custom transport behavior implements only its domain handler; it must still route persistence through `settings_manager_set`.
- **Every HTTP route is documented, in the same change that adds it (rev 2.3).**
  `components/HTTP_API.md` owns the conventions and the central route map; a
  component adding or changing any `/api/*` (or other reserved-namespace)
  route updates that map **and** its own per-endpoint reference (its
  `HTTP_API.md`, or its `README.md` for a trivial single route) in the same
  commit. The web UI is built against these files: an undocumented route
  does not exist as far as the product is concerned.

## 6b. Console commands (CLI)

Console commands follow the same ownership inversion as HTTP routes and
settings: **`cmdline_manager` owns the registry and the transports;
components own their commands.**

- A component with console commands ships them in `<comp>_cli.c` with a
  `<comp>_register_cli()` that registers via `cmdline_manager_register()`
  (esp_console semantics; `help`/`hint` strings static: they feed the
  legacy-format `help`). Handlers print with `cmdline_printf()` /
  `cmdline_write()` and run in the dispatcher/console context: treat the
  §2 corollary as binding (exec_line callers have INTERNAL stacks; a
  handler may therefore touch flash/LittleFS).
- **The component registers itself**: `<comp>_register_cli()` is called
  from the component's own settings `on_apply`, gated by a **`cli` bool
  field (default true)** in its settings: reboot-to-apply, like every
  setting. `main` wires no per-component CLI calls. Registration failure
  is log-and-continue, never an `on_apply` failure.
- A component with no other settings still gets the knob: a minimal
  `{cli}` field-table descriptor. If its `init` runs before
  `settings_manager_init`, expose `<comp>_register_settings()` for main
  to call right after settings init (the `log_manager_register_settings`
  pattern).
- Components BELOW `cmdline_manager` in the graph (log_manager,
  settings_manager) cannot register: that would be a cycle; their knobs,
  composites spanning multiple components (`system`), and pending stubs
  live in main's `main_cli.c` (the `api_http` pattern).
- **Legacy option parity**: commands that existed in the legacy firmware
  keep their argtable option interfaces byte-alike (outputs + `OK`
  trailers); bare invocation gives a v6 summary; v6 additions are options.
- Every command lands in `cmdline_manager/README.md`'s ownership table in
  the same change: the CLI analogue of the §6 HTTP-route rule.

## 6c. BLE GATT surface (rev 2.8)

Third-party apps are built against the device's BLE interface exactly as
the web UI is built against `HTTP_API.md`, so the same-change rule
applies:

- **`components/ble_manager/BLE_API.md` is the on-air contract**: every
  service, characteristic, UUID, property, security requirement, the MTU
  and chunking rules, and the **stream-channel registry table** (which
  16-bit UUID pair carries which protocol, and the setting that makes it
  present).
- `ble_manager` owns the GATT table and the transport (`ble_manager_channel_*`
  API: pre-start registration, one RX StreamBuffer per channel, blocking
  credit-waited notify writes); a component that carries a protocol over
  a channel owns its **protocol document** (`<comp>/<NAME>_PROTOCOL.md`,
  or its README for a trivial pipe) with the framing, every message, the
  flow-control rule, the error vocabulary and byte-exact examples that
  double as host-test vectors. `BLE_API.md` links to it.
- Any change that adds, removes or re-orders a characteristic, changes a
  UUID, a permission flag, an MTU/payload cap or the semantics of bytes on
  an existing pipe updates `BLE_API.md` **in the same commit**; a channel
  owner updates its protocol document likewise. An undocumented
  characteristic does not exist as far as the product is concerned.
- **Characteristics are only ever appended** within a service, never
  inserted or re-ordered: bonded phones cache attribute handles, and an
  append keeps the legacy handles valid (the doc records the consequence
  for already-bonded clients: they must re-discover to see the additions).
- Channel protocols are byte streams over a reliable in-order link:
  frames MUST carry their own length, MAY span several ATT units, and a
  receiver reassembles by that length, never by unit boundaries. The
  channel registry is bounded (§12: `ble_manager_channel_capacity()` on
  the `WICAN CAPS` line).

## 7. Testing

**Every component ships tests.** Two layers, both required where they apply:

- **Unit (Unity, C):** pure logic, schema validation, settings serialize/deserialize, frame encode/decode, parsers, state machines. Run on the **host (`linux` target)** so CI needs no hardware and they're fast. Live in `components/<comp>/test/` (or `host_test/`).
- **pytest (pytest-embedded):** builds/flashes a test app and asserts on DUT serial via `dut.expect(...)`. Covers on-target/integration behaviour and anything needing a real peripheral, flash, or LittleFS. QEMU where possible, hardware where not. Lives in `components/<comp>/test_apps/` with `pytest_<comp>.py`.

Rules:

- Anything that *can* be host-tested *must* be: don't require a board to test a parser.
- Settings: schema + `on_validate` + `on_migrate` get unit tests (valid accepted, invalid rejected with the expected error, every historical version migrates to current). Atomic store, rollback-on-corruption, and the boot-failure fallback (§4.3 steps 4 to 5) get pytest/target tests.
- No component logic merges without tests covering it. CI runs host unit tests on every push.
- **Tests listen for error lines (rev 2.6).** The measured baseline is ZERO `E` lines across every suite, even deliberate-failure scenarios (wrong PSK, dead AP) surface as I/W. On-target runs report `errors=N warnings=N` in their verdict (`serial_capture.py`); the wifi HIL suite fails any test over its error budget (default 0, per-test `ERROR_BUDGET` entries only for paths that genuinely must log at ERROR: tallies persist in `hil_health.log`); the composed-firmware bench (`system_bench.py` step 6) asserts `log_errors == 0`, flash budgets, registry headroom, task-stack headroom ≥ 256 B, and ZERO latched fault codes. A new test that produces error lines is either finding a bug (fix it) or mis-leveled logging (§10).

**Performance-truth rules (rev 2.7 candidate, 2026-07-21: written in
the blood of the TWAI-port drain-loop bug, which shipped a 30x
rx_count error past every functional test):**

- **Counter conservation.** Any driver/pipeline with counters gets a
  bench that asserts them against a KNOWN generator: put exactly N
  frames/bytes/packets on the wire, assert `received == N` and every
  loss is accounted in a named counter. "No crash + plausible traffic"
  is not a pass. (Model: the datalog byte-exact bench; now required
  for CAN, WiFi and USB paths.)
- **Test at saturation.** A peripheral driver (or port of one) is not
  verified until it has run at the medium's full line rate: light
  functional traffic hides event/ISR mis-handling entirely (the drain
  loop was invisible at 186 f/s, obvious at 4.2k f/s).
- **A deterministic unexplained counter delta FAILS the bench.** A
  counter that moves by an exact arithmetic multiple of the workload
  is a bug with a confession attached: never park it as noise.
  (rx_missed == 2x transactions was the drain loop announcing itself.)
- **Per-event ISR/callback APIs are called once per event.** No drain
  loops around `*_from_isr`/callback receive calls unless the API's
  contract explicitly defines an empty-return; verify the contract in
  the header, not by assumption.
- **Verify config-dependent behavior at RUNTIME, not by config file.**
  Duplicate/regenerated sdkconfig entries silently resolved a core-
  affinity A/B onto the wrong core; the boot `esp_intr_dump()` caught
  it. A bench that A/Bs a config must assert the active value on the
  device (intr table, log line, API field).
- **Performance verdicts need n >= 2 per side.** Single-run A/B deltas
  (2924-missed "win", 198<->3363 f/s ATMA swings) were run variance.

**On-device test apps use the main firmware's configuration.** A pytest test app exists to prove the component works **on the shipped device**, so it must build against the main firmware's partition table and sdkconfig: never a private fork of them:

- The test app inherits the main project's sdkconfig (list the main project's defaults file first in `SDKCONFIG_DEFAULTS`, then at most a small test-only overlay), and its `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` points at the repo-root partition table the main app builds with. Test apps do **not** carry their own `partitions.csv`.
- If the component needs a Kconfig option or a partition the main firmware doesn't have yet (a new data partition, a `CONFIG_SPIRAM_*` option, a bigger `max_uri_handlers`), **change the main firmware's sdkconfig / partition table first**, then let the test app inherit it. The main config is the single source of truth; a test app must never build with config the shipped firmware doesn't have.
- The test-only overlay may contain only options that are meaningless in production (test console/runner options, log verbosity for `dut.expect`), each with a one-line justification comment. Functional config (memory, partitions, drivers, stack sizes) never goes in the overlay.
- Rationale: a test that passes on its own partition layout or config proves nothing about the real device, and config drift between test apps and firmware is where "works in the test app, crashes in the field" comes from.

**External tools / hardware-in-the-loop.** A test may rely on external tooling: PCAN, Ollie v2, bench PSUs, custom jigs, etc. If it does, that test app **must ship a `README.md`** next to it (`components/<comp>/test_apps/README.md`) covering:

- **Tools + versions**: exact instrument and any firmware/driver version (e.g. PCAN-USB + driver, Ollie v2 + its FW rev).
- **Wiring**: physical connections (CAN-H/CAN-L, power, target pins), bus termination, and bitrate.
- **Host setup**: drivers, packages, and the tool's CLI/config needed to drive or observe the DUT (e.g. `python-can` backend, the Ollie command set).
- **Procedure**: bring-up steps, how to launch the test, and the **pass/fail criteria** (expected frames, timing, serial output).

Rule: if a human or AI can't reproduce the bench from that README alone, it's incomplete. Tests that silently assume a tool is present and configured are not allowed.

## 8. Contribution rules (human + AI)

- Match this document over any habit or training default. If a change conflicts with it, the document wins.
- New component → new directory containing: public header, `CMakeLists.txt`, `idf_component.yml`, `README.md` (§3 contents), lifecycle funcs (`init`/`start`/`stop`, no `_deinit`), settings in `<comp>_settings.c` if it has any (field-table schema + descriptor + hooks, §4.1), log descriptor (§9.2), CLI commands (if any) per §6b (`<comp>_cli.c`, self-registered on settings apply behind the `cli` bool, documented in cmdline_manager's ownership table), `Kconfig` if it has build-time options, host unit tests + a pytest test app built on the main firmware's partition table and sdkconfig (§7).
- No new file over 700 lines. No non-Allman braces. No settings persistence outside the Settings Manager. No hand-written JSON Schema strings: field tables only, in `<comp>_settings.c` (§4.1). No log routing outside the Log Manager. No logging from ISRs.
- No `ESP_ERROR_CHECK`/panic paths in components or `main` (§3): log and degrade; test apps only. New/changed HTTP routes update `components/HTTP_API.md` + the component's endpoint reference in the same change (§6). New/changed BLE characteristics or channel protocols update `ble_manager/BLE_API.md` + the owner's protocol document in the same change, appending characteristics only (§6c).
- Default to static allocation in PSRAM. Internal RAM only for the cases in §2, justified by comment. No flash writes from PSRAM-stack tasks.
- Public API or schema change → note it in the component header; if the persisted shape changed, bump `version` **and** extend `on_migrate` **and** add the migration unit test.

## 9. Debug Log Manager

The Log Manager owns log routing the same way the Settings Manager owns persistence: **one component owns the pipeline; everything else registers into it.** No component writes to a socket, file, or ring buffer directly for logging.

### 9.1 Capture: hook, don't migrate

The manager installs itself via `esp_log_set_vprintf()`. Components keep using plain `ESP_LOGx(TAG, ...)`: no new logging API, no call-site migration, and IDF-internal / third-party logs are captured too. Do not invent wrapper macros.

### 9.2 Component registration

```c
typedef struct
{
    const char      *name;           // must equal the component's TAG (§10)
    esp_log_level_t  default_level;  // level at boot before settings apply
} log_descriptor_t;

esp_err_t log_manager_register(const log_descriptor_t *desc);
esp_err_t log_manager_set_level(const char *name, esp_log_level_t level); // runtime, ephemeral
```

Registration builds the per-component level table, so an operator can set `can` to DEBUG while everything else stays at INFO: at runtime, without a reboot. This is the sanctioned exception in §4.2: a runtime knob, not a setting.

### 9.3 Sinks

```c
typedef struct
{
    const char *name;
    esp_err_t (*write)(const char *line, size_t len);  // called from the log task only
} log_sink_t;

esp_err_t log_manager_add_sink(const log_sink_t *sink);
```

- The **console (UART) sink is built in and always available from `log_manager_init()`.**
- Multiple sinks may be active simultaneously (console + TCP + flash ring buffer), each independently enabled/disabled at runtime.
- A sink's `write` runs in the log task's context, never in the producer's: a slow sink can therefore never stall a producer.

### 9.4 Settings vs runtime knobs

The Log Manager registers its **own settings descriptor** (§4) for persisted defaults: per-component boot levels, which sinks auto-enable, sink parameters (TCP port, ring size). Those apply at boot like all settings. `log_manager_set_level()` and runtime sink enable/disable are **ephemeral**: they reset to persisted defaults on reboot. Turning on DEBUG to chase a bug must not require a reboot, and must not survive one.

### 9.5 Pipeline, backpressure, and context rules

- Producers format into a fixed-depth message queue; a dedicated log task drains it to the sinks.
- **Backpressure = drop-oldest, never block.** The producer path must be wait-free: if the queue is full, the oldest entry is dropped and a drop counter increments; the manager emits a `dropped N messages` line when pressure clears. Blocking a CAN task on a stalled TCP client is not acceptable; losing old debug lines is.
- **No logging from ISR context.** `ESP_LOGx` from an ISR is forbidden (it takes locks, and per §2 an IRAM ISR can run with the cache off). An ISR that must report posts a value to a queue; a task logs it.
- **Memory placement:** queue and buffers live in PSRAM per §2. The log task performs no flash writes itself; a flash-backed sink hands writes to an internal-stack writer path (§2 corollary).
- A **flash-backed crash ring buffer** (retrieve last N lines after reboot) is planned but out of scope for v1; when built, its writes follow §4.4-style atomicity.

### 9.6 Boot order

`log_manager_init()` is the **first** component init in `main`, before `settings_manager_init()`, so settings-apply errors (§4.3) have somewhere to go. Until then, output falls through to the raw IDF console (default vprintf); no early-boot buffering is required. The manager starts with compile-time defaults; its own `on_apply` later in boot adjusts levels and sinks to the persisted configuration.

## 10. Logging conventions

- One `TAG` per component, equal to the component/directory name exactly: `static const char *TAG = "settings_manager";`. This is what makes §9.2's per-component level control line up.
- Levels: `ESP_LOGE` only for failures that are also returned/propagated as errors; `ESP_LOGW` for fallback events (defaults applied, migration failed, messages dropped); `ESP_LOGI` for lifecycle milestones (started, connected): a handful per boot, not per operation; `ESP_LOGD`/`ESP_LOGV` for everything chatty.
- **No logging in hot paths at INFO or above** (per-frame CAN RX, per-request HTTP). Use DEBUG so it filters out in normal operation.
- **Retry/reconnect loops log at DEBUG too.** Anything that repeats on a timer while a condition persists (WiFi reconnect attempts every 5 s, per-request 404s, client aborts) floods the pipeline at INFO/WARN. Log the state *transition* once at INFO ("connected", "disconnected"), the *attempts* at DEBUG.
- **Why this matters mechanically:** producers never block on a sink, the Log Manager queues and drops-oldest (§9.5), so a burst can't hang the device, but it CAN evict the lines you actually needed and (pre-`log_manager_start()`, when routing is synchronous to the UART) slow early boot. Sinks differ in tolerance: UART at 115200 sustains ~1 kline/min, TCP/UDP more, the PSRAM ring is fast but small. The level rules above are what keep every sink usable.
- **Every component registers a `log_descriptor_t`** in its `init` (§9.2) so per-TAG runtime levels work. Exception: `settings_manager` (the Log Manager depends on it for its own settings descriptor, registering back would be a cycle); `main` registers `{"settings_manager", ESP_LOG_INFO}` during composition.
- Never log secrets (WiFi passwords, tokens, API keys) at any level.
- **`ESP_LOGE` is load-bearing (rev 2.6).** The health net counts every E line: any error during boot latches a `boot_errors` fault code, `system_bench` asserts `log_errors == 0`, and the HIL suite fails any test whose error count exceeds its budget (default ZERO: the measured baseline across all 14 wifi scenarios, including wrong-PSK and dead-AP ones). Consequences: an EXPECTED failure (retry that will self-heal, deliberate negative path, peer offline) logs `W` or `D`, never `E`; conversely, don't downgrade a genuine fault to W to sneak past the net: fix the fault. The two error lines the net caught on day one were both real bugs.

## 11. Flash-write discipline (rev 2.6)

Born from a real bug: the settings boot pass rewrote **all 37 component files to LittleFS on every boot** for weeks (4.8 s of an 8 s boot plus needless NOR wear) invisible because every write "worked". Normative rules:

- **Boot-path writes MUST be change-guarded.** Anything that runs unconditionally at boot (settings apply, cache warm-up, table load) may only write when the data actually changed (migration ran, schema-fill added keys, content differs). "Persist to be safe" is the bug, not the safety.
- **Periodic writes MUST be gated** by an `enabled` setting (data_logger pattern) or debounced/change-guarded (a timer that rewrites an unchanged blob is the same bug on a schedule). Event-driven writes (user action, API PUT, OTA, pairing) are fine.
- **Latched/recurring state goes to NVS** (wear-leveled), with explicit wear discipline: write on first occurrence, bound the update rate (the fault store writes at most once per code per boot, a fault firing 10k times writes flash once).
- **The counters keep everyone honest:** `CONFIG_SPI_FLASH_ENABLE_COUNTERS` is on; `dev_status_manager_flash()` exposes ops-since-boot; boot prints `WICAN FLASH …`; breaches latch `boot_flash_budget`/`flash_churn` faults; the bench asserts a boot-erase budget and idle-flash-quiet. A steady-state boot measures **0 writes / 0 erases**: that is the bar. If your component moves those numbers, justify it in its README's memory/footprint section.
- When touching a chip with **mode-gated registers** (standby vs active), a successful I2C ACK does NOT mean the register was written: verify critical config writes with a read-back (§3; the AW2023 channel-enable write was silently discarded for the chip's entire v6 life).

## 12. Bounded registries & the health surface (rev 2.6)

Fixed-size tables fail silently at the worst time (the settings registry overflow degraded two components for days; the cmdline table silently dropped a command; the bridge endpoint table sat at 15/16). Rules:

- **Overflow must be loud AND latched:** log `ESP_LOGE` (the boot_errors net catches it) and degrade per §3, never abort.
- **Registries with cross-component registrants expose occupancy** via a `<comp>_capacity(used, cap)` getter, wired by main into the `WICAN CAPS` boot line and `/api/status.health.caps`. The bench asserts **headroom ≥ 2** on every registry it knows; main latches a `registry_headroom` fault below that.
- **Adding a registrant to any bounded table ⇒ check its headroom in the same change** (and grep a boot log for register failures). Headroom in PSRAM tables is cheap: size for growth.
- **Device fault codes** (`dev_status_manager_fault_raise`) are the automotive-DTC layer: structural problems latch to NVS and survive reboot/power-cycle until manually cleared (`faults -c`, `POST /api/faults/clear`). Components below dev_status in the dependency graph (settings, cmdline, log) must NOT call it (cycles): their `ESP_LOGE` **is** their raise path via the boot_errors net; main owns the structural raise points. A component above dev_status MAY raise its own codes for latch-worthy conditions; keep codes stable, short, and documented in the component README.
