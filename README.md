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
- Global timeout and slow-release settings shared by all runtime combos.
- Name and combo arrays stored through
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings).
- React Web UI for listing and editing combo slots.

## Storage Format

The combo body is saved as one packed byte-array custom setting per combo slot:

```text
version, flags, position_count, layer_mask, behavior_id, param1, param2, positions[]
```

`positions[]` are 16-bit values, and behavior parameters are 32-bit values. The
name is saved separately as a string-array custom setting with the same index.
Timeout and slow-release mode are saved as separate global custom settings and
apply to every runtime combo. The global settings RPC response also reports
`max_combo`, the maximum number of combo slots configured in the firmware.

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
       - name: zmk
         remote: cormoran
         revision: main+custom-studio-protocol
         import:
           file: app/west.yml
   ```

   This module imports `zmk-feature-custom-settings`. The custom Studio RPC Web
   UI requires ZMK from cormoran's `main+custom-studio-protocol` branch.

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

Set global timeout and slow-release mode in the Global Settings panel. Enable the
corresponding persist checkbox before saving if the setting should survive
reboot. The panel also shows the maximum number of runtime combo slots available
on the connected firmware.

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
