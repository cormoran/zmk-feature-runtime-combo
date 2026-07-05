# zmk-feature-runtime-combo

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-runtime-combo/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-runtime-combo/actions/workflows/zmk-module.yml)

Runtime Combo is a ZMK module that adds combos which can be edited at runtime
through the unofficial custom ZMK Studio RPC protocol.

A combo watches multiple key positions. When all configured positions are pressed
within the combo timeout, the configured ZMK behavior is invoked. Each combo is
stored by numeric slot internally and can also have a display name for the Web UI.

## Features

- Runtime editable combo slots.
- Separate RPC methods for combo content and combo names to keep request payloads
  small.
- Compact binary storage for combo bodies.
- Global timeout, slow-release, and require-prior-idle settings shared by all
  runtime combos, with per-combo overrides for all three.
- Native-parity overlap resolution: if one combo's positions are a subset of
  another enabled combo's positions, the longer combo always wins.
- Name and combo arrays stored through
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings).
- React Web UI for listing and editing combo slots.

## Storage Format

The combo body is saved as one packed byte-array custom setting per combo slot:

```text
version, flags, position_count, layer_mask, behavior_id, param1, param2,
timeout_ms, require_prior_idle_ms, positions[]
```

`positions[]` are 16-bit values, and behavior parameters are 32-bit values.
`timeout_ms` and `require_prior_idle_ms` are per-combo overrides; `0` means
inherit the corresponding global setting. The `flags` byte also carries a
tri-state slow-release override (inherit / on / off). The name is saved
separately as a string-array custom setting with the same index. Timeout,
slow-release, and require-prior-idle mode are saved as separate global custom
settings and apply to every runtime combo unless overridden. The global
settings RPC response also reports `max_combo`, the maximum number of combo
slots configured in the firmware.

Records written by older module versions (storage version 2, without the
per-combo override fields) are still read correctly; they behave as if every
override is left at "inherit".

## Overlap Resolution

If pressing a set of keys exactly completes one combo, but a second enabled
combo also matches every currently pressed position and needs one or more
additional positions, the shorter combo does not fire immediately. It only
fires once no longer combo remains viable: because an unrelated key is pressed,
the timeout expires, or one of the pressed keys is released. Whichever combo
ends up firing always uses only its own positions; any positions pressed while
waiting on a longer candidate are replayed as normal key presses if that
candidate never completes.

## User Guide

1. Add the module to `config/west.yml`.

   ```yml
   manifest:
     remotes:
       - name: cormoran
         url-base: https://github.com/cormoran
     projects:
       - name: zmk-feature-runtime-combo
         remote: cormoran
         revision: main
         import: true
   ```

   This module imports `zmk-feature-custom-settings`.

2. Enable the feature in `config/<shield>.conf`.

   ```conf
   CONFIG_ZMK_RUNTIME_COMBO=y

   # Required for the custom Studio RPC Web UI
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_RUNTIME_COMBO_STUDIO_RPC=y
   CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
   CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES=96
   CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
   ```

3. Open the Web UI from the custom subsystem URL or run it locally:

   ```bash
   cd web
   npm install
   npm run dev
   ```

4. Connect to the keyboard, choose a slot, and set:

   - Name: display name only.
   - Positions: comma-separated key positions, such as `0, 1`.
   - Behavior ID, Param 1, Param 2: the same binding fields ZMK Studio uses.
   - Layer mask: `0` means all layers.
   - Timeout ms / Require prior idle ms: `0` inherits the corresponding global
     setting; a nonzero value overrides it for this combo only.
   - Slow release: `Inherit global` follows the Global Settings panel; `On`/`Off`
     overrides it for this combo only.

Set global timeout, slow-release mode, and require-prior-idle time in the
Global Settings panel. Require-prior-idle suppresses a combo if a non-modifier
key was tapped within that many milliseconds before the combo's first key press
(useful for avoiding accidental combos while typing fast); `0` disables it.
Enable the corresponding persist checkbox before saving if the setting should
survive reboot. The panel also shows the maximum number of runtime combo slots
available on the connected firmware.

## Development

```bash
# Run unit test + build test and verify results
python3 -m unittest

# Run build test directly
west zmk-build tests/zmk-config

# Run unit test directly
west zmk-test tests -m .

# Run web tests
cd web && npm test
```

The Web protobuf TypeScript files are generated with:

```bash
cd web
npm run generate
```
