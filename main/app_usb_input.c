#include "app_usb_input.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

enum { KEY_EVENTS = 64, MOUSE_EVENTS = 32, DELTA_LIMIT = 4096 };
typedef struct { uint32_t key; bool down; } key_event_t;
typedef struct { lv_obj_t *root, *previous_focus; lv_group_t *group; } modal_scope_t;
static struct {
    portMUX_TYPE lock;
    atomic_bool ready, enabled, gated;
    app_power_t *power;
    lv_display_t *display;
    lv_indev_t *pointer, *keypad;
    lv_obj_t *cursor;
    lv_group_t *group;
    modal_scope_t modals[8];
    unsigned modal_count, failed_modals;
    bool (*extra_gate)(void *);
    void *gate_context;
    uint8_t keys[32], blocked_keys[32], buttons, blocked_buttons;
    uint32_t logical[256];
    key_event_t key_queue[KEY_EVENTS];
    uint8_t mouse_queue[MOUSE_EVENTS];
    unsigned key_head, key_count, mouse_head, mouse_count;
    int32_t dx, dy, wheel, x, y;
    uint32_t current_key;
    bool caps, reset_requested, mouse_present, key_down;
    bool capture_keys;
    uint8_t captured_usage;
} s = {.lock = portMUX_INITIALIZER_UNLOCKED};

static bool bit(const uint8_t *keys, unsigned usage)
{ return (keys[usage / 8U] & (1U << (usage % 8U))) != 0; }

/* Standard USB keyboard US layout. HID reports usages, not characters; other
 * national layouts/IMEs need an explicit layout layer rather than guessed text. */
static uint32_t key_value(unsigned usage, const uint8_t *keys, bool caps)
{
    const bool shift = bit(keys, 0xE1) || bit(keys, 0xE5);
    if (bit(keys, 0xE0) || bit(keys, 0xE2) || bit(keys, 0xE3) ||
        bit(keys, 0xE4) || bit(keys, 0xE6) || bit(keys, 0xE7)) return 0;
    if (usage >= 4 && usage <= 29) return (shift != caps ? 'A' : 'a') + usage - 4U;
    if (usage >= 30 && usage <= 39) return (uint8_t)(shift ? "!@#$%^&*()" : "1234567890")[usage - 30U];
    if (usage >= 45 && usage <= 56) return (uint8_t)(shift ? "_+{}|~:\"~<>?" : "-=[]\\#;'`,./")[usage - 45U];
    if (usage >= 89 && usage <= 97) return '1' + usage - 89U;
    switch (usage) {
        case 40: case 88: return LV_KEY_ENTER;
        case 41: return LV_KEY_ESC;
        case 42: return LV_KEY_BACKSPACE;
        case 43: return shift ? LV_KEY_PREV : LV_KEY_NEXT;
        case 44: return ' ';
        case 74: return LV_KEY_HOME;
        case 77: return LV_KEY_END;
        case 76: return LV_KEY_DEL;
        case 75: case 82: return LV_KEY_UP;
        case 78: case 81: return LV_KEY_DOWN;
        case 79: return LV_KEY_RIGHT;
        case 80: return LV_KEY_LEFT;
        case 84: return '/'; case 85: return '*';
        case 86: return '-'; case 87: return '+';
        case 98: return '0'; case 99: return '.';
        default: return 0;
    }
}

static int32_t bounded_add(int32_t a, int32_t b)
{
    int32_t result = a + b;
    return result < -DELTA_LIMIT ? -DELTA_LIMIT : result > DELTA_LIMIT ? DELTA_LIMIT : result;
}

/* Under s.lock. Dropping an overfull queue cancels its gesture instead of
 * inventing a click or leaving the last keyboard/button press stuck forever. */
static void consume_locked(void)
{
    for (unsigned i = 0; i < sizeof(s.keys); ++i) s.blocked_keys[i] |= s.keys[i];
    s.blocked_buttons |= s.buttons;
    memset(s.logical, 0, sizeof(s.logical));
    s.key_count = s.mouse_count = s.key_head = s.mouse_head = 0;
    s.dx = s.dy = s.wheel = 0;
    s.reset_requested = true;
}

static bool queue_key(uint32_t key, bool down)
{
    if (!key) return true;
    if (s.key_count == KEY_EVENTS) return false;
    s.key_queue[(s.key_head + s.key_count++) % KEY_EVENTS] = (key_event_t){key, down};
    return true;
}

void app_usb_input_publish(void *context, const aether_usb_input_event_t *event)
{
    (void)context;
    if (!event || !atomic_load(&s.ready)) return;
    const bool blocked = !atomic_load(&s.enabled) || atomic_load(&s.gated) ||
        (s.power && app_power_is_sleeping(s.power));
    portENTER_CRITICAL(&s.lock);
    if (event->type == AETHER_USB_INPUT_RESET) {
        memset(s.keys, 0, sizeof(s.keys)); memset(s.blocked_keys, 0, sizeof(s.blocked_keys));
        s.buttons = s.blocked_buttons = 0; s.mouse_present = false;
        s.captured_usage = 0;
        consume_locked();
    } else if (event->type == AETHER_USB_INPUT_KEYBOARD) {
        uint8_t old[32], old_blocked[32];
        memcpy(old, s.keys, sizeof(old)); memcpy(old_blocked, s.blocked_keys, sizeof(old_blocked));
        memcpy(s.keys, event->keys, sizeof(s.keys));
        for (unsigned i = 0; i < sizeof(s.keys); ++i) s.blocked_keys[i] &= s.keys[i];
        if (blocked) consume_locked();
        else if (s.capture_keys) {
            if (!s.captured_usage) {
                /* HID keyboard usages 1–3 denote report errors, not keys. */
                for (unsigned u = 4; u <= 0xE7; ++u) {
                    if (bit(s.keys, u) && !bit(old, u) && !bit(s.blocked_keys, u)) {
                        s.captured_usage = (uint8_t)u;
                        break;
                    }
                }
            }
            /* A captured Enter or Esc must never activate another widget,
             * including after the binding dialog closes while it is held. */
            for (unsigned i = 0; i < sizeof(s.keys); ++i) s.blocked_keys[i] |= s.keys[i];
        } else {
            bool overflow = false;
            /* Publish releases first so simultaneous key swaps don't inherit
             * the old Enter press/release on a newly focused widget. */
            for (unsigned u = 0; u < 224; ++u) if (bit(old, u) && !bit(s.keys, u)) {
                if (!bit(old_blocked, u)) overflow |= !queue_key(s.logical[u], false);
                s.logical[u] = 0;
            }
            if (bit(s.keys, 57) && !bit(old, 57) && !bit(s.blocked_keys, 57)) s.caps = !s.caps;
            for (unsigned u = 0; u < 224; ++u) if (bit(s.keys, u) && !bit(old, u) && !bit(s.blocked_keys, u)) {
                s.logical[u] = key_value(u, s.keys, s.caps);
                overflow |= !queue_key(s.logical[u], true);
            }
            if (overflow) consume_locked();
        }
    } else if (event->type == AETHER_USB_INPUT_MOUSE) {
        const uint8_t old = s.buttons;
        s.buttons = event->buttons; s.blocked_buttons &= s.buttons; s.mouse_present = true;
        if (blocked) consume_locked();
        else {
            s.dx = bounded_add(s.dx, event->dx); s.dy = bounded_add(s.dy, event->dy);
            s.wheel = bounded_add(s.wheel, event->wheel);
            if ((old ^ s.buttons) & 1U) {
                if (s.mouse_count == MOUSE_EVENTS) consume_locked();
                else s.mouse_queue[(s.mouse_head + s.mouse_count++) % MOUSE_EVENTS] = s.buttons & ~s.blocked_buttons;
            }
        }
    }
    portEXIT_CRITICAL(&s.lock);
}

/* Called only by the two LVGL read callbacks. */
static void reset_devices(void)
{
    lv_indev_reset(s.pointer, NULL); lv_indev_reset(s.keypad, NULL);
    /* LVGL reset clears pointer bookkeeping, but not keypad.last_state. */
    lv_indev_wait_release(s.pointer); lv_indev_wait_release(s.keypad);
    s.key_down = false; s.current_key = 0;
}

static bool gate_input(void)
{
    const bool blocked = !atomic_load(&s.enabled) || (s.power && app_power_is_sleeping(s.power)) ||
        (s.extra_gate && s.extra_gate(s.gate_context));
    atomic_store(&s.gated, blocked);
    portENTER_CRITICAL(&s.lock);
    if (blocked) consume_locked();
    const bool reset = s.reset_requested;
    s.reset_requested = false;
    portEXIT_CRITICAL(&s.lock);
    if (reset) reset_devices();
    return blocked;
}

static void scroll_wheel(int32_t wheel)
{
    lv_point_t point = {s.x, s.y};
    lv_obj_t *object = lv_indev_search_obj(lv_display_get_layer_sys(s.display), &point);
    if (!object) object = lv_indev_search_obj(lv_display_get_layer_top(s.display), &point);
    if (!object) object = lv_indev_search_obj(lv_display_get_screen_active(s.display), &point);
    while (object) {
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_top(object) > 0 || lv_obj_get_scroll_bottom(object) > 0)) {
            lv_obj_scroll_by_bounded(object, 0, wheel * 36, LV_ANIM_OFF);
            break;
        }
        object = lv_obj_get_parent(object);
    }
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const bool blocked = gate_input();
    portENTER_CRITICAL(&s.lock);
    const int32_t dx = s.dx, dy = s.dy, wheel = s.wheel;
    s.dx = s.dy = s.wheel = 0;
    uint8_t buttons = s.buttons & ~s.blocked_buttons;
    if (s.mouse_count) { buttons = s.mouse_queue[s.mouse_head]; s.mouse_head = (s.mouse_head + 1) % MOUSE_EVENTS; --s.mouse_count; }
    const bool more = s.mouse_count != 0, present = s.mouse_present;
    portEXIT_CRITICAL(&s.lock);
    if (blocked || !present) lv_obj_add_flag(s.cursor, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s.cursor, LV_OBJ_FLAG_HIDDEN);
    if (!blocked) {
        s.x += dx; s.y += dy;
        const int32_t width = lv_display_get_horizontal_resolution(s.display), height = lv_display_get_vertical_resolution(s.display);
        if (s.x < 0) s.x = 0; else if (s.x >= width) s.x = width - 1;
        if (s.y < 0) s.y = 0; else if (s.y >= height) s.y = height - 1;
        if (wheel) scroll_wheel(wheel);
        if (dx || dy || wheel || buttons) lv_display_trigger_activity(s.display);
    }
    data->point = (lv_point_t){s.x, s.y};
    data->state = !blocked && (buttons & 1U) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !blocked && more;
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const bool blocked = gate_input();
    portENTER_CRITICAL(&s.lock);
    if (!blocked) while (s.key_count) {
        const key_event_t event = s.key_queue[s.key_head];
        /* LVGL has one active keypad key. Release it before a new key without
         * losing a fast press/release that arrived between polling intervals. */
        if (event.down && s.key_down && event.key != s.current_key) { s.key_down = false; break; }
        s.key_head = (s.key_head + 1) % KEY_EVENTS; --s.key_count;
        if (!event.down && event.key != s.current_key) continue;
        s.current_key = event.key; s.key_down = event.down;
        break;
    }
    const bool more = s.key_count != 0;
    portEXIT_CRITICAL(&s.lock);
    data->key = s.current_key;
    data->state = !blocked && s.key_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !blocked && more;
    if (!blocked && s.key_down) lv_display_trigger_activity(s.display);
}

esp_err_t app_usb_input_init(app_power_t *power, lv_display_t *display)
{
    if (!display) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&s.ready)) return ESP_ERR_INVALID_STATE;
    lv_group_t *previous = lv_group_get_default();
    s.group = lv_group_create(); s.pointer = lv_indev_create(); s.keypad = lv_indev_create();
    s.cursor = lv_obj_create(lv_display_get_layer_sys(display));
    if (!s.group || !s.pointer || !s.keypad || !s.cursor) {
        if (s.cursor) lv_obj_delete(s.cursor);
        if (s.pointer) lv_indev_delete(s.pointer);
        if (s.keypad) lv_indev_delete(s.keypad);
        if (s.group) lv_group_delete(s.group);
        s.cursor = NULL; s.pointer = s.keypad = NULL; s.group = NULL;
        lv_group_set_default(previous);
        return ESP_ERR_NO_MEM;
    }
    s.power = power; s.display = display;
    s.x = lv_display_get_horizontal_resolution(display) / 2;
    s.y = lv_display_get_vertical_resolution(display) / 2;
    lv_obj_set_size(s.cursor, 12, 12);
    lv_obj_remove_flag(s.cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.cursor, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(s.cursor, lv_color_hex(0x91DDFF), 0);
    lv_obj_set_style_bg_opa(s.cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s.cursor, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s.cursor, 1, 0);
    lv_obj_set_style_radius(s.cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s.cursor, 0, 0);
    lv_indev_set_type(s.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_type(s.keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(s.pointer, display); lv_indev_set_display(s.keypad, display);
    lv_indev_set_read_cb(s.pointer, pointer_read); lv_indev_set_read_cb(s.keypad, keypad_read);
    lv_indev_set_cursor(s.pointer, s.cursor); lv_indev_set_group(s.keypad, s.group);
    lv_timer_set_period(lv_indev_get_read_timer(s.pointer), 8);
    lv_timer_set_period(lv_indev_get_read_timer(s.keypad), 8);
    lv_group_set_default(s.group);
    atomic_store(&s.enabled, true); atomic_store(&s.ready, true);
    return ESP_OK;
}

void app_usb_input_set_gate(bool (*blocked)(void *), void *context)
{ s.extra_gate = blocked; s.gate_context = context; }

void app_usb_input_set_enabled(bool enabled)
{
    atomic_store(&s.enabled, enabled);
    if (!enabled && atomic_load(&s.ready)) {
        portENTER_CRITICAL(&s.lock); consume_locked(); portEXIT_CRITICAL(&s.lock);
    }
}

bool app_usb_input_capture_keys(bool enable)
{
    if (!atomic_load(&s.ready)) return false;
    portENTER_CRITICAL(&s.lock);
    if (s.capture_keys != enable) {
        s.capture_keys = enable;
        s.captured_usage = 0;
        consume_locked();
    }
    portEXIT_CRITICAL(&s.lock);
    return true;
}

uint8_t app_usb_input_take_captured_usage(void)
{
    if (!atomic_load(&s.ready)) return 0;
    portENTER_CRITICAL(&s.lock);
    const uint8_t usage = s.captured_usage;
    s.captured_usage = 0;
    portEXIT_CRITICAL(&s.lock);
    return usage;
}

static bool inside(lv_obj_t *object, lv_obj_t *root)
{
    for (; object; object = lv_obj_get_parent(object)) if (object == root) return true;
    return false;
}

static void modal_cancel_input(void)
{
    portENTER_CRITICAL(&s.lock); consume_locked(); s.reset_requested = false; portEXIT_CRITICAL(&s.lock);
    reset_devices();
}

static void modal_bind_keypad(void)
{
    lv_indev_set_group(s.keypad, s.failed_modals ? NULL : s.modal_count ?
        s.modals[s.modal_count - 1].group : s.group);
}

static void modal_deleted(lv_event_t *event);
static void modal_failed_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != lv_event_get_current_target_obj(event)) return;
    if (s.failed_modals) --s.failed_modals;
    modal_bind_keypad(); modal_cancel_input();
}

static unsigned modal_remove_failures(lv_obj_t *object)
{
    unsigned count = lv_obj_remove_event_cb_with_user_data(object, modal_failed_deleted, NULL);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += modal_remove_failures(lv_obj_get_child(object, i));
    return count;
}

static void modal_release(lv_obj_t *modal, bool deleting)
{
    if (!modal || !atomic_load(&s.ready)) return;
    const unsigned failed = modal_remove_failures(modal);
    if (failed) { s.failed_modals = failed > s.failed_modals ? 0 : s.failed_modals - failed; }
    bool changed = failed != 0;
    lv_obj_t *restore = NULL;
    /* Descendant scopes close with a hidden/deleted parent; sibling notices
     * remain independent, including during an entire page's deletion. */
    for (unsigned i = s.modal_count; i-- > 0;) {
        modal_scope_t scope = s.modals[i];
        if (!inside(scope.root, modal)) continue;
        const bool was_top = i == s.modal_count - 1;
        for (unsigned j = i + 1; j < s.modal_count; ++j)
            if (s.modals[j].previous_focus && lv_obj_is_valid(s.modals[j].previous_focus) &&
                inside(s.modals[j].previous_focus, scope.root))
                s.modals[j].previous_focus = scope.previous_focus;
        memmove(&s.modals[i], &s.modals[i + 1], (s.modal_count - i - 1) * sizeof(s.modals[0]));
        --s.modal_count;
        lv_obj_remove_event_cb_with_user_data(scope.root, modal_deleted, NULL);
        if (!deleting) while (lv_group_get_obj_count(scope.group))
            lv_group_add_obj(s.group, lv_group_get_obj_by_index(scope.group, 0));
        /* On DELETE the widgets are about to disappear; the group clears
         * their membership without reintroducing them into the base group. */
        lv_group_delete(scope.group);
        if (was_top) restore = scope.previous_focus;
        changed = true;
    }
    if (changed) {
        modal_bind_keypad();
        if (!s.failed_modals && restore && lv_obj_is_valid(restore) && lv_obj_is_visible(restore) &&
            lv_obj_get_group(restore) == lv_indev_get_group(s.keypad)) lv_group_focus_obj(restore);
        modal_cancel_input();
    }
}

static void modal_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
        modal_release(lv_event_get_target_obj(event), true);
}

static void modal_collect(lv_obj_t *object, lv_group_t *group)
{
    /* Refreshing a parent panel must not steal controls from a nested dialog. */
    for (unsigned i = 0; i < s.modal_count; ++i)
        if (s.modals[i].root == object && s.modals[i].group != group) return;
    if (lv_obj_get_group(object) && lv_obj_get_group(object) != group) lv_group_add_obj(group, object);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) modal_collect(lv_obj_get_child(object, i), group);
}

esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *preferred_focus)
{
    if (!modal) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s.ready)) return ESP_OK;
    for (unsigned i = 0; i < s.modal_count; ++i) if (s.modals[i].root == modal) {
        /* A paged list can replace its rows while open. New controls initially
         * join the base group; refreshing the scope imports only those rows. */
        modal_collect(modal, s.modals[i].group);
        if (preferred_focus && lv_obj_get_group(preferred_focus) == s.modals[i].group) lv_group_focus_obj(preferred_focus);
        return ESP_OK;
    }
    lv_group_t *group = s.modal_count < sizeof(s.modals) / sizeof(s.modals[0]) ? lv_group_create() : NULL;
    if (!group) {
        ++s.failed_modals;
        lv_obj_add_event_cb(modal, modal_failed_deleted, LV_EVENT_DELETE, NULL);
        modal_bind_keypad(); modal_cancel_input();
        return ESP_ERR_NO_MEM;
    }
    lv_group_t *previous = lv_indev_get_group(s.keypad);
    s.modals[s.modal_count++] = (modal_scope_t){.root = modal, .group = group,
        .previous_focus = previous ? lv_group_get_focused(previous) : NULL};
    lv_obj_add_event_cb(modal, modal_deleted, LV_EVENT_DELETE, NULL);
    modal_collect(modal, group);
    lv_group_set_wrap(group, true);
    modal_bind_keypad();
    if (preferred_focus && lv_obj_get_group(preferred_focus) == group) lv_group_focus_obj(preferred_focus);
    modal_cancel_input();
    return ESP_OK;
}

void app_usb_input_modal_close(lv_obj_t *modal) { modal_release(modal, false); }
