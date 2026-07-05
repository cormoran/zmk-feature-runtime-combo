# Design: Flexibility and Usability Improvements for Runtime Combo

Status: proposal (not implemented)

This document analyzes the current limitations of `zmk-feature-runtime-combo`
and proposes a design to make the module more flexible and general-purpose for
end users. Implementation is expected to be done in follow-up work, phase by
phase (see [Implementation plan](#implementation-plan)).

## Current architecture summary

- Each combo lives in a numbered slot (`CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS`,
  default 8, Kconfig range 1–16).
- A combo body is stored as one packed byte-array custom setting per slot
  (storage version 2):
  `version, flags, position_count, reserved, layer_mask(u32), behavior_local_id(u16), param1(u32), param2(u32), positions[](u16)`.
- Display names are a parallel string-array custom setting.
- `timeout_ms` and `slow_release` are **global** custom settings shared by all
  combos.
- The event listener decodes every slot from the custom-settings registry on
  every key position event (`count_candidates_for_position`,
  `find_complete_combo`, `update_timeout_task` each iterate and unpack all
  slots).
- All configuration happens at runtime through the custom Studio RPC Web UI.
  There is no way to define combos in the `.keymap` / devicetree.

## Limitations to address

| # | Limitation | Impact |
|---|------------|--------|
| L1 | No compile-time (devicetree/`.keymap`) default combos | Users must re-enter every combo through the Web UI after flashing; a settings reset (`settings_erase`, flash re-partition) silently loses all combos; keyboard vendors cannot ship sensible defaults; combos cannot be version-controlled in a zmk-config repo. |
| L2 | Timeout and slow-release are global only | Native ZMK combos support per-combo `timeout-ms`, `require-prior-idle-ms` and `slow-release`. Users mixing "fast chord" combos with "deliberate multi-key" combos cannot tune them independently. |
| L3 | `require-prior-idle` protection is effectively inert | `is_quick_tap()` compares `last_tapped_timestamp > timestamp` with no idle window, which is almost never true. Fast typists get accidental combo triggers that native ZMK's `require-prior-idle-ms` would prevent. |
| L4 | Overlapping combos resolve differently from native ZMK | `find_complete_combo()` fires the first exact match immediately. If combo A = `{0,1}` and combo B = `{0,1,2}` are both defined, B can never trigger. Native ZMK keeps a fully-pressed candidate pending while a strict superset candidate is still viable and picks the candidate with the most positions. |
| L5 | Per-event decode of all slots | Every position event performs up to `3 × MAX_COMBOS` registry lookups + unpacks. Fine at 8 slots, but blocks raising the slot count and adds latency on the hot key-event path. Global settings are also re-read (registry lookup) multiple times per event. |
| L6 | Slot count capped at 16 | Kconfig `range 1 16`. Power users routinely define dozens of combos in native ZMK. |
| L7 | Delete only disables | `zmk_runtime_combo_delete()` writes an `enabled=false` record. There is no way to distinguish "explicitly disabled" from "empty slot", which also matters for L1 default-fallback semantics. |
| L8 | Minor API friction | `zmk_runtime_combo_write_name()` fails for `index >= zmk_runtime_combo_count()`, so a name cannot be set before the combo body. Raw behavior IDs / position numbers in the Web UI are hard to use. |

## Proposal 1 (P0): Compile-time default combos in devicetree

This is the primary request. Users define default combos in the `.keymap` (or
any devicetree overlay); they work out of the box and can still be overridden,
disabled, or restored at runtime.

### User-facing syntax

Mirror the native `zmk,combos` binding so users can copy existing combo
definitions with minimal edits:

```dts
/ {
    runtime_combo_defaults {
        compatible = "cormoran,runtime-combo-defaults";

        combo_esc {
            key-positions = <0 1>;
            bindings = <&kp ESC>;
            layers = <0 1>;           // optional; omitted = all layers
            display-name = "Esc";     // optional; defaults to node name
            slot = <0>;               // optional; defaults to definition order
            timeout-ms = <30>;        // optional per-combo override (Proposal 2)
            require-prior-idle-ms = <125>; // optional (Proposal 2)
            slow-release;             // optional (Proposal 2)
        };

        combo_tab {
            key-positions = <2 3>;
            bindings = <&kp TAB>;
        };
    };
};
```

New file `dts/bindings/cormoran,runtime-combo-defaults.yaml` describing the
child binding (same property set as `zmk,combos` plus `slot` and
`display-name`).

### Build-time extraction

Native ZMK proves all required information is available statically:
`app/src/combo.c` resolves the behavior device name at compile time with
`ZMK_KEYMAP_EXTRACT_BINDING(0, n)` (which expands to `DEVICE_DT_NAME(...)`).
The module generates:

```c
struct zmk_runtime_combo_default {
    uint8_t slot;
    struct zmk_runtime_combo_config config; /* behavior_dev = static string */
    const char *name;
};
static const struct zmk_runtime_combo_default dt_defaults[] = { ... };
```

- `layers` array → `layer_mask` via a `DT_FOREACH_PROP_ELEM` fold of `BIT()`;
  absent property → `0` (all layers).
- `slot` defaults to the child index (definition order).
- `BUILD_ASSERT` that every slot is `< CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS`
  and that no two children claim the same slot.
- The whole node is optional; without it the module behaves exactly as today.

Note: the packed storage format embeds the **runtime-assigned behavior local
id**, so defaults cannot be expressed as `zmk_custom_setting.default_value`
byte blobs (those are static initializers). The default table therefore stays
outside the custom-settings registry and is consulted at read time.

### Runtime semantics: read-path fallback with three slot states

Effective value of slot *i*:

1. **Set** — stored bytes are non-empty (version-2/3 record, possibly with
   `enabled=false`): use the stored record. This covers both runtime-created
   combos and runtime-*disabled* defaults (L7's tombstone becomes a feature:
   "delete" on a default slot stores an `enabled=false` record, which
   suppresses the default).
2. **Unset + DT default exists** — stored bytes empty (`size == 0`): fall back
   to `dt_defaults` for that slot. Same fallback for the name array.
3. **Unset, no default** — empty slot as today.

A new **reset** operation erases the stored bytes (via
`zmk_custom_setting_reset()` on the slot element), returning the slot to
state 2/3. This cleanly separates "disable" (write tombstone) from "restore
default" (erase override).

Why read-path fallback instead of seeding defaults into memory at init:

- Init-time `WRITE_MODE_MEMORY` seeding would make every default look like an
  unsaved change; the existing `Discard` RPC (which restores
  persistent/default values) would wipe the seeded defaults until reboot.
- Fallback keeps the stored format and save/discard semantics untouched and
  costs one table lookup on the (to-be-cached, see Proposal 4) read path.

### API and RPC additions

```c
/* Number of DT-defined defaults (0 if node absent). */
uint32_t zmk_runtime_combo_default_count(void);
/* Read the DT default for a slot, -ENOENT if none. */
int zmk_runtime_combo_read_default(uint32_t index, struct zmk_runtime_combo_config *combo);
/* Erase the stored override so the slot falls back to its default / empty. */
int zmk_runtime_combo_reset(uint32_t index);
```

Proto changes (`runtime_combo.proto`):

```proto
enum ComboSource {
    COMBO_SOURCE_EMPTY = 0;      // no stored value, no default
    COMBO_SOURCE_DEFAULT = 1;    // DT default, not overridden
    COMBO_SOURCE_OVERRIDDEN = 2; // DT default exists but stored value wins
    COMBO_SOURCE_RUNTIME = 3;    // stored value, no DT default
}

message Combo {
    // existing fields...
    ComboSource source = 9;
}

message ResetComboRequest { uint32 index = 1; }
```

`ResetComboRequest` joins the request oneof; the Web UI shows a
default/overridden badge per slot and a "Reset to default" button.

## Proposal 2 (P1): Per-combo overrides — storage version 3

Bring the per-combo knobs to parity with native `zmk,combos` while keeping the
global values as the inherited baseline:

- `timeout_ms` (u16): `0` = inherit global.
- `require_prior_idle_ms` (u16): `0` = inherit global (new global setting,
  default `0` = disabled). This replaces the inert `is_quick_tap()` check with
  native semantics: skip the combo when
  `last_tapped_timestamp + require_prior_idle_ms > now` (L3).
- flags gains two bits: `SLOW_RELEASE_OVERRIDE` (bit 1) and
  `SLOW_RELEASE_VALUE` (bit 2), giving a tri-state (inherit / on / off).

Packed layout version 3 (header grows 18 → 22 bytes):

```text
offset  size  field
0       1     version = 3
1       1     flags (bit0 enabled, bit1 slow_release_override, bit2 slow_release_value)
2       1     position_count
3       1     reserved
4       4     layer_mask
8       2     behavior_local_id
10      4     param1
14      4     param2
18      2     timeout_ms        (0 = inherit global)
20      2     require_prior_idle_ms (0 = inherit global)
22      2*n   positions
```

Compatibility: `packed_to_combo()` keeps accepting version 2 (fields default to
"inherit"); writes always emit version 3. No migration pass needed — records
upgrade on next write. `RUNTIME_COMBO_PACKED_MAX_SIZE` grows by 4 bytes; the
existing `BUILD_ASSERT` against `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`
still guards the fit.

Additions: new fields on `struct zmk_runtime_combo_config`, matching proto
fields on `Combo` / `SetComboRequest` (tri-state slow-release as an enum), a
global `require_prior_idle_ms` custom setting + `SetRequirePriorIdleMsRequest`,
and Web UI inputs with an explicit "inherit global" state.

Timeout evaluation with per-combo timeouts: `combo_matches_pending()` and
`update_timeout_task()` use each candidate's *effective* timeout instead of the
single global value (the candidate-min logic in `update_timeout_task()` already
supports differing deadlines).

## Proposal 3 (P1): Native-parity overlap resolution

Adopt the native ZMK candidate algorithm so subset/superset combos coexist
(L4):

- When the pending keys fully match a combo but a strict superset candidate is
  still viable within its timeout, do not fire immediately; remember it as the
  *fully-pressed candidate*.
- Fire the fully-pressed candidate (choosing the candidate with the most
  positions) when: a key is pressed that no larger candidate accepts, the
  timeout expires, or any pending key is released.
- If no candidate remains after a press, replay pending keys as today.

This changes observable behavior only for overlapping definitions; a combo with
no superset still fires on the final keydown exactly as today. State cost: one
`int` for the fully-pressed candidate index. No storage or RPC impact, so this
can ship independently of Proposals 1–2.

## Proposal 4 (P2): Decoded-combo cache and higher slot limits

- Keep a RAM cache of decoded, *effective* combos (after default fallback):
  `struct zmk_runtime_combo_config` minus the oversized `int32_t` positions
  array (use `uint16_t` positions in the cache entry) plus a validity bit.
  Rebuild a slot's entry on write / delete / reset / discard / save, and all
  slots on settings load. The key-event hot path then never touches the
  custom-settings registry or unpacking code.
- Cache the global settings struct the same way (invalidate on write/discard).
- Raise `ZMK_RUNTIME_COMBO_MAX_COMBOS` range to `1 63` (the packed
  `layer_mask`/slot bookkeeping and `ZMK_VIRTUAL_KEY_POSITION_COMBO` offsets
  stay well within range; 63 slots ≈ 63 × ~60 B ≈ 3.8 KB RAM for the cache —
  document the RAM cost in Kconfig help so users on nRF52 can budget).
- `zmk_runtime_combo_count()` no longer bounds iteration on the hot path; the
  cache holds `MAX_COMBOS` entries with validity flags.

## Proposal 5 (P2): RPC / Web UI usability

- Fix L8: allow `zmk_runtime_combo_write_name()` for any
  `index < MAX_COMBOS` by growing both arrays together (mirror
  `ensure_name_array_size` for the combo array).
- Web UI: render key positions as a clickable layout using the standard ZMK
  Studio core RPC (`keymap.get_keymap` provides positions and layer names) and
  list behaviors by name via `behaviors.list_all_behaviors` +
  `get_behavior_details`, replacing raw numeric entry. The custom subsystem
  web app already speaks the Studio transport, so this is UI-only work.
- Show `source` badges and per-combo override indicators (Proposals 1–2).

## Out of scope (considered, rejected for now)

- **Multiple bindings per combo**: native combos invoke exactly one binding;
  macros already cover multi-action needs.
- **Import/export of combo sets**: already provided generically by
  `zmk-feature-custom-settings` unified export.
- **Storing behavior names instead of local ids**: costs ~16 bytes/slot and
  local ids are already stable across reboots via
  `ZMK_BEHAVIOR_LOCAL_IDS`.

## Implementation plan

Per repo convention, each phase follows proto → firmware handler → web UI, with
unit tests in `tests/<case>` and build coverage in `tests/zmk-config`.

1. **Phase 1 — DT defaults (Proposal 1)**: binding yaml, static extraction
   macros, read-path fallback, `reset` API, `ComboSource` + `ResetComboRequest`
   RPC, Web UI badge/reset button.
   Tests: unit test with a `runtime_combo_defaults` node in `tests/test.dtsi`
   (default fires; override wins; delete suppresses default; reset restores);
   build test enabling defaults in `tests/zmk-config`.
2. **Phase 2 — storage v3 + per-combo overrides (Proposal 2)**: packed v3,
   v2 read compatibility, global `require_prior_idle_ms`, effective-value
   resolution, proto + Web UI tri-state controls.
   Tests: v2 record still decodes; per-combo timeout beats global;
   require-prior-idle blocks combos after a tap.
3. **Phase 3 — overlap resolution (Proposal 3)**: candidate tracking, longest
   fully-pressed selection.
   Tests: subset+superset pair (fast third key → superset; timeout → subset;
   release → subset).
4. **Phase 4 — cache + limits (Proposal 4)**: decoded cache with invalidation
   hooks, Kconfig range bump, RAM cost documentation.
5. **Phase 5 — Web UI usability (Proposal 5)**: layout-based position picker,
   behavior name listing, name-before-body fix.

Phases 1 and 3 are independent; Phase 2 builds on Phase 1's proto churn being
settled; Phase 4 should land after 1–2 so the cache caches effective values.

## Open questions

1. Should `delete` on a runtime-only slot (no DT default) erase instead of
   tombstone? Proposed: yes — erase when no default exists, tombstone when one
   does; the distinction is invisible to users but keeps storage tidy.
2. Kconfig upper bound (32 vs 63) — pick based on measured RAM after Phase 4's
   cache sizing on nRF52840.
3. Should DT defaults also seed the global `timeout_ms` / `slow_release`
   (e.g. properties on the container node)? Proposed: yes, as optional
   container-level properties mapping to the custom settings' defaults — cheap
   to add in Phase 1.
