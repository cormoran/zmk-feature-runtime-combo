/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define ZMK_RUNTIME_COMBO_SUBSYSTEM_ID "cormoran__runtime_combo"
#define ZMK_RUNTIME_COMBO_COMBOS_KEY "combos"
#define ZMK_RUNTIME_COMBO_NAMES_KEY "names"

struct zmk_runtime_combo_config {
    bool enabled;
    bool slow_release;
    uint8_t key_position_len;
    uint16_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    int32_t key_positions[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
};

int zmk_runtime_combo_read(uint32_t index, struct zmk_runtime_combo_config *combo);
int zmk_runtime_combo_read_name(uint32_t index, char *name, size_t name_size);
uint32_t zmk_runtime_combo_count(void);
uint32_t zmk_runtime_combo_max_count(void);
int zmk_runtime_combo_write(uint32_t index, const struct zmk_runtime_combo_config *combo,
                            bool persist);
int zmk_runtime_combo_write_name(uint32_t index, const char *name, bool persist);
int zmk_runtime_combo_delete(uint32_t index, bool persist);
