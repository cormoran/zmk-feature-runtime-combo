/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <cormoran/runtime_combo/runtime_combo.h>
#include <cormoran/zmk/custom_settings.h>

#include <dt-bindings/zmk/keys.h>
#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RUNTIME_COMBO_STORAGE_VERSION 2
#define RUNTIME_COMBO_FLAG_ENABLED BIT(0)
#define RUNTIME_COMBO_PACKED_HEADER_SIZE 18
#define RUNTIME_COMBO_PACKED_MAX_SIZE                                                              \
    (RUNTIME_COMBO_PACKED_HEADER_SIZE +                                                            \
     (CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO * sizeof(uint16_t)))

BUILD_ASSERT(RUNTIME_COMBO_PACKED_MAX_SIZE <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
             "Runtime combo packed value must fit in one custom setting bytes value");

#define RUNTIME_COMBO_EMPTY_BYTES                                                                  \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0})

#define RUNTIME_COMBO_ARRAY_ELEMENT_DEFINE(_name, _key, _index, _value_type, _default_value)       \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {                    \
        ZMK_CUSTOM_SETTING_NO_CONSTRAINT};                                                         \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,                                     \
        .key = _key "/" ZMK_CUSTOM_SETTINGS_STRINGIFY(_index),                                     \
        .array_key = _key,                                                                         \
        .array_index = (_index),                                                                   \
        .array_max_size = CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS,                                     \
        .default_array_size = 0,                                                                   \
        .array_size = 0,                                                                           \
        .persistent_array_size = 0,                                                                \
        .value_type = _value_type,                                                                 \
        .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                          \
        .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                 \
        .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = _default_value,                                                           \
    }

#define RUNTIME_COMBO_DEFINE_COMBO_SETTING(n, _)                                                   \
    RUNTIME_COMBO_ARRAY_ELEMENT_DEFINE(runtime_combo_##n, ZMK_RUNTIME_COMBO_COMBOS_KEY, n,         \
                                       ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                        \
                                       RUNTIME_COMBO_EMPTY_BYTES);

#define RUNTIME_COMBO_DEFINE_NAME_SETTING(n, _)                                                    \
    RUNTIME_COMBO_ARRAY_ELEMENT_DEFINE(runtime_combo_name_##n, ZMK_RUNTIME_COMBO_NAMES_KEY, n,     \
                                       ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                       \
                                       ZMK_CUSTOM_SETTING_VALUE_STRING(""));

LISTIFY(CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, RUNTIME_COMBO_DEFINE_COMBO_SETTING, (), 0)
LISTIFY(CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS, RUNTIME_COMBO_DEFINE_NAME_SETTING, (), 0)

static const struct zmk_custom_setting_constraint runtime_combo_timeout_ms_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 1},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 65535}}},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, runtime_combo_timeout_ms) = {
    .custom_subsystem_id = ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
    .key = ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = runtime_combo_timeout_ms_constraints,
    .constraints_count = ARRAY_SIZE(runtime_combo_timeout_ms_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                      .int32_value = CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_TIMEOUT_MS},
};

static const struct zmk_custom_setting_constraint runtime_combo_slow_release_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, runtime_combo_slow_release) = {
    .custom_subsystem_id = ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
    .key = ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY,
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = runtime_combo_slow_release_constraints,
    .constraints_count = ARRAY_SIZE(runtime_combo_slow_release_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = false},
};

struct runtime_combo_active {
    uint16_t combo_idx;
    uint8_t key_positions_pressed_count;
    struct zmk_position_state_changed_event
        key_positions_pressed[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
};

static uint8_t pending_count;
static struct zmk_position_state_changed_event
    pending_keys[CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO];
static struct runtime_combo_active active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS];
static uint8_t active_combo_count;
static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;
static int64_t last_tapped_timestamp = INT32_MIN;
static int64_t last_combo_timestamp = INT32_MIN;

static void put_u16(uint8_t *dest, uint16_t value) { sys_put_le16(value, dest); }
static void put_u32(uint8_t *dest, uint32_t value) { sys_put_le32(value, dest); }
static uint16_t get_u16(const uint8_t *src) { return sys_get_le16(src); }
static uint32_t get_u32(const uint8_t *src) { return sys_get_le32(src); }

static const struct zmk_custom_setting *combo_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                 ZMK_RUNTIME_COMBO_COMBOS_KEY, index);
}

static const struct zmk_custom_setting *name_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                 ZMK_RUNTIME_COMBO_NAMES_KEY, index);
}

uint32_t zmk_runtime_combo_count(void) {
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_COMBOS_KEY);
    return setting ? zmk_custom_setting_array_size(setting) : 0;
}

uint32_t zmk_runtime_combo_max_count(void) { return CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS; }

static int packed_to_combo(const struct zmk_custom_setting_value *value,
                           struct zmk_runtime_combo_config *combo) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES) {
        return -EINVAL;
    }
    if (value->size == 0) {
        *combo = (struct zmk_runtime_combo_config){0};
        return 0;
    }
    if (value->size < RUNTIME_COMBO_PACKED_HEADER_SIZE ||
        value->bytes_value[0] != RUNTIME_COMBO_STORAGE_VERSION) {
        return -EINVAL;
    }

    uint8_t key_position_len = value->bytes_value[2];
    if (key_position_len < 2 ||
        key_position_len > CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO ||
        value->size != RUNTIME_COMBO_PACKED_HEADER_SIZE + key_position_len * sizeof(uint16_t)) {
        return -EINVAL;
    }

    *combo = (struct zmk_runtime_combo_config){
        .enabled = (value->bytes_value[1] & RUNTIME_COMBO_FLAG_ENABLED) != 0,
        .key_position_len = key_position_len,
        .layer_mask = get_u32(&value->bytes_value[4]),
        .behavior =
            {
                .param1 = get_u32(&value->bytes_value[10]),
                .param2 = get_u32(&value->bytes_value[14]),
            },
    };

    zmk_behavior_local_id_t behavior_id = get_u16(&value->bytes_value[8]);
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (!behavior_name) {
        return -ENODEV;
    }
    combo->behavior.behavior_dev = behavior_name;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    combo->behavior.local_id = behavior_id;
#endif

    for (uint8_t i = 0; i < key_position_len; i++) {
        combo->key_positions[i] =
            get_u16(&value->bytes_value[RUNTIME_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)]);
    }
    return 0;
}

static int combo_to_packed(const struct zmk_runtime_combo_config *combo,
                           struct zmk_custom_setting_value *value) {
    if (combo->key_position_len < 2 ||
        combo->key_position_len > CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO) {
        return -EINVAL;
    }
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    int ret = zmk_behavior_validate_binding(&combo->behavior);
    if (ret < 0) {
        return ret;
    }

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] < 0 || combo->key_positions[i] > UINT16_MAX) {
            return -ERANGE;
        }
    }

    *value = (struct zmk_custom_setting_value){
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = RUNTIME_COMBO_PACKED_HEADER_SIZE + combo->key_position_len * sizeof(uint16_t),
    };
    value->bytes_value[0] = RUNTIME_COMBO_STORAGE_VERSION;
    value->bytes_value[1] = combo->enabled ? RUNTIME_COMBO_FLAG_ENABLED : 0;
    value->bytes_value[2] = combo->key_position_len;
    value->bytes_value[3] = 0;
    put_u32(&value->bytes_value[4], combo->layer_mask);
    put_u16(&value->bytes_value[8], behavior_id);
    put_u32(&value->bytes_value[10], combo->behavior.param1);
    put_u32(&value->bytes_value[14], combo->behavior.param2);

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        put_u16(&value->bytes_value[RUNTIME_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)],
                combo->key_positions[i]);
    }
    return 0;
}

int zmk_runtime_combo_read(uint32_t index, struct zmk_runtime_combo_config *combo) {
    if (!combo || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_COMBOS_KEY, index, &value);
    if (ret < 0) {
        return ret;
    }
    return packed_to_combo(&value, combo);
}

int zmk_runtime_combo_read_name(uint32_t index, char *name, size_t name_size) {
    if (!name || name_size == 0 || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_COMBO_NAMES_KEY, index, &value);
    if (ret < 0) {
        name[0] = '\0';
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }
    strncpy(name, value.string_value, name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

int zmk_runtime_combo_read_global_settings(struct zmk_runtime_combo_global_settings *settings) {
    if (!settings) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value timeout_value;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY, &timeout_value);
    if (ret < 0) {
        return ret;
    }
    if (timeout_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
        timeout_value.int32_value < 1 || timeout_value.int32_value > UINT16_MAX) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value slow_release_value;
    ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_COMBO_SUBSYSTEM_ID,
                                         ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY, &slow_release_value);
    if (ret < 0) {
        return ret;
    }
    if (slow_release_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return -EINVAL;
    }

    *settings = (struct zmk_runtime_combo_global_settings){
        .timeout_ms = timeout_value.int32_value,
        .slow_release = slow_release_value.bool_value,
    };
    return 0;
}

static int ensure_name_array_size(uint32_t index, bool persist) {
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    if (zmk_custom_setting_array_size(setting) > index) {
        return 0;
    }
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(
        setting, &ZMK_CUSTOM_SETTING_VALUE_STRING(""), array_size,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_write(uint32_t index, const struct zmk_runtime_combo_config *combo,
                            bool persist) {
    if (!combo || index >= CONFIG_ZMK_RUNTIME_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = combo_to_packed(combo, &value);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *setting = combo_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    ret = zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                 persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                         : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    return ensure_name_array_size(index, persist);
}

int zmk_runtime_combo_write_name(uint32_t index, const char *name, bool persist) {
    if (!name || index >= zmk_runtime_combo_count()) {
        return -EINVAL;
    }
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    };
    strncpy(value.string_value, name, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    value.string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
    value.size = strlen(value.string_value);

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                  persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                          : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_write_timeout_ms(uint16_t timeout_ms, bool persist) {
    if (timeout_ms == 0) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(timeout_ms);
    return zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_TIMEOUT_MS_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_write_slow_release(bool slow_release, bool persist) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_BOOL(slow_release);
    return zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_COMBO_SUBSYSTEM_ID, ZMK_RUNTIME_COMBO_SLOW_RELEASE_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_combo_delete(uint32_t index, bool persist) {
    struct zmk_runtime_combo_config combo;
    int ret = zmk_runtime_combo_read(index, &combo);
    if (ret < 0) {
        return ret;
    }
    combo.enabled = false;
    return zmk_runtime_combo_write(index, &combo, persist);
}

static bool combo_contains_position(const struct zmk_runtime_combo_config *combo,
                                    int32_t position) {
    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool combo_active_on_layer(const struct zmk_runtime_combo_config *combo, uint8_t layer) {
    return combo->layer_mask == 0 || (combo->layer_mask & BIT(layer)) != 0;
}

static struct zmk_runtime_combo_global_settings current_global_settings(void) {
    struct zmk_runtime_combo_global_settings settings = {
        .timeout_ms = CONFIG_ZMK_RUNTIME_COMBO_DEFAULT_TIMEOUT_MS,
        .slow_release = false,
    };
    if (zmk_runtime_combo_read_global_settings(&settings) < 0) {
        LOG_WRN("Using default runtime combo global settings");
    }
    return settings;
}

static bool combo_matches_pending(const struct zmk_runtime_combo_config *combo, int64_t timestamp) {
    if (!combo->enabled || pending_count == 0) {
        return false;
    }
    uint16_t timeout_ms = current_global_settings().timeout_ms;
    if (pending_keys[0].data.timestamp + timeout_ms <= timestamp) {
        return false;
    }
    for (uint8_t i = 0; i < pending_count; i++) {
        if (!combo_contains_position(combo, pending_keys[i].data.position)) {
            return false;
        }
    }
    return true;
}

static bool is_quick_tap(const struct zmk_runtime_combo_config *combo, int64_t timestamp) {
    ARG_UNUSED(combo);
    return last_tapped_timestamp > last_combo_timestamp && last_tapped_timestamp > timestamp;
}

static int read_enabled_combo(uint32_t index, struct zmk_runtime_combo_config *combo) {
    int ret = zmk_runtime_combo_read(index, combo);
    if (ret < 0 || !combo->enabled) {
        return ret < 0 ? ret : -ENOENT;
    }
    return 0;
}

static int count_candidates_for_position(int32_t position, int64_t timestamp) {
    int count = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
    uint32_t combo_count = zmk_runtime_combo_count();

    for (uint32_t i = 0; i < combo_count; i++) {
        struct zmk_runtime_combo_config combo;
        if (read_enabled_combo(i, &combo) < 0) {
            continue;
        }
        if (!combo_contains_position(&combo, position) ||
            !combo_active_on_layer(&combo, highest_active_layer) ||
            is_quick_tap(&combo, timestamp)) {
            continue;
        }
        if (pending_count == 0 || combo_matches_pending(&combo, timestamp)) {
            count++;
        }
    }
    return count;
}

static int release_pending_keys(void) {
    uint8_t count = pending_count;
    pending_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (i == 0) {
            ZMK_EVENT_RELEASE(pending_keys[i]);
        } else {
            ZMK_EVENT_RAISE(pending_keys[i]);
        }
    }
    return count;
}

static void cleanup_pending(void) {
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    release_pending_keys();
}

static void update_timeout_task(void) {
    if (pending_count == 0) {
        k_work_cancel_delayable(&timeout_task);
        timeout_task_timeout_at = 0;
        return;
    }

    int64_t timeout_at = LLONG_MAX;
    uint16_t timeout_ms = current_global_settings().timeout_ms;
    uint32_t combo_count = zmk_runtime_combo_count();
    for (uint32_t i = 0; i < combo_count; i++) {
        struct zmk_runtime_combo_config combo;
        if (read_enabled_combo(i, &combo) < 0 || !combo_matches_pending(&combo, k_uptime_get())) {
            continue;
        }
        timeout_at = MIN(timeout_at, pending_keys[0].data.timestamp + timeout_ms);
    }

    if (timeout_at == LLONG_MAX) {
        cleanup_pending();
        return;
    }
    if (timeout_task_timeout_at != timeout_at &&
        k_work_schedule(&timeout_task, K_MSEC(MAX(timeout_at - k_uptime_get(), 0))) >= 0) {
        timeout_task_timeout_at = timeout_at;
    }
}

static int press_combo_behavior(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo,
                                int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN + combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    last_combo_timestamp = timestamp;
    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static int release_combo_behavior(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo,
                                  int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN + combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static struct runtime_combo_active *store_active_combo(uint32_t combo_idx) {
    if (active_combo_count >= CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS) {
        LOG_WRN("Unable to store runtime combo; active combo limit reached");
        return NULL;
    }

    struct runtime_combo_active *active = &active_combos[active_combo_count++];
    *active = (struct runtime_combo_active){.combo_idx = combo_idx};
    for (uint8_t i = 0; i < pending_count; i++) {
        active->key_positions_pressed[i] = pending_keys[i];
    }
    active->key_positions_pressed_count = pending_count;
    return active;
}

static int activate_combo(uint32_t combo_idx, const struct zmk_runtime_combo_config *combo) {
    struct runtime_combo_active *active = store_active_combo(combo_idx);
    if (!active) {
        cleanup_pending();
        return ZMK_EV_EVENT_CAPTURED;
    }
    pending_count = 0;
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    press_combo_behavior(combo_idx, combo, active->key_positions_pressed[0].data.timestamp);
    return ZMK_EV_EVENT_CAPTURED;
}

static int find_complete_combo(struct zmk_runtime_combo_config *combo) {
    uint32_t combo_count = zmk_runtime_combo_count();
    for (uint32_t i = 0; i < combo_count; i++) {
        if (read_enabled_combo(i, combo) < 0 || combo->key_position_len != pending_count ||
            !combo_matches_pending(combo, pending_keys[pending_count - 1].data.timestamp)) {
            continue;
        }
        return i;
    }
    return -ENOENT;
}

static int position_state_down(struct zmk_position_state_changed *data) {
    int candidates = count_candidates_for_position(data->position, data->timestamp);
    if (candidates == 0) {
        if (pending_count > 0) {
            cleanup_pending();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (pending_count >= CONFIG_ZMK_RUNTIME_COMBO_MAX_POSITIONS_PER_COMBO) {
        cleanup_pending();
        return ZMK_EV_EVENT_BUBBLE;
    }

    pending_keys[pending_count++] = copy_raised_zmk_position_state_changed(data);

    struct zmk_runtime_combo_config combo;
    int complete_combo = find_complete_combo(&combo);
    if (complete_combo >= 0) {
        return activate_combo(complete_combo, &combo);
    }

    update_timeout_task();
    return ZMK_EV_EVENT_CAPTURED;
}

static void deactivate_combo(uint8_t active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        active_combos[active_combo_index] = active_combos[active_combo_count];
    }
    active_combos[active_combo_count] = (struct runtime_combo_active){0};
}

static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (uint8_t active_idx = 0; active_idx < active_combo_count; active_idx++) {
        struct runtime_combo_active *active = &active_combos[active_idx];
        struct zmk_runtime_combo_config combo;
        if (zmk_runtime_combo_read(active->combo_idx, &combo) < 0) {
            deactivate_combo(active_idx);
            return true;
        }

        bool key_released = false;
        bool all_keys_pressed = active->key_positions_pressed_count == combo.key_position_len;
        bool all_keys_released = true;
        for (uint8_t i = 0; i < active->key_positions_pressed_count; i++) {
            if (key_released) {
                active->key_positions_pressed[i - 1] = active->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else {
                key_released = true;
            }
        }

        if (!key_released) {
            continue;
        }

        active->key_positions_pressed_count--;
        bool slow_release = current_global_settings().slow_release;
        if ((slow_release && all_keys_released) || (!slow_release && all_keys_pressed)) {
            release_combo_behavior(active->combo_idx, &combo, timestamp);
        }
        if (all_keys_released) {
            deactivate_combo(active_idx);
        }
        return true;
    }
    return false;
}

static int position_state_up(struct zmk_position_state_changed *data) {
    if (pending_count > 0) {
        cleanup_pending();
    }
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    ARG_UNUSED(item);
    if (timeout_task_timeout_at != 0 && k_uptime_get() >= timeout_task_timeout_at) {
        cleanup_pending();
    }
}

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBO_TEST)
static bool runtime_combo_test_setup_done;

static int runtime_combo_test_setup(void) {
    if (runtime_combo_test_setup_done) {
        return 0;
    }

    const char *key_press = "key_press";
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(key_press);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        LOG_ERR("Runtime combo test cannot find key_press behavior");
        return -ENODEV;
    }

    struct zmk_runtime_combo_config combo = {
        .enabled = true,
        .key_position_len = 2,
        .behavior = {.behavior_dev = key_press, .param1 = B},
        .key_positions = {0, 1},
    };

    int ret = zmk_runtime_combo_write(0, &combo, false);
    if (ret < 0) {
        LOG_ERR("Runtime combo test setup failed: %d", ret);
        return ret;
    }

    runtime_combo_test_setup_done = true;
    LOG_INF("PASS: runtime_combo_test_setup");
    return 0;
}
#endif

static int position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(eh);
    if (!data) {
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_ZMK_RUNTIME_COMBO_TEST)
    runtime_combo_test_setup();
#endif
    return data->state ? position_state_down(data) : position_state_up(data);
}

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int runtime_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    }
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(runtime_combo, runtime_combo_listener);
ZMK_SUBSCRIPTION(runtime_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(runtime_combo, zmk_keycode_state_changed);

static int runtime_combo_init(void) {
    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    return 0;
}

SYS_INIT(runtime_combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
