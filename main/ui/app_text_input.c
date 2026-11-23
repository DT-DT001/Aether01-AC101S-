#include "app_text_input.h"
#include "app_ui_internal.h"
#include "app_otg_control.h"
#include "app_pinyin.h"
#include "app_pinyin_swipe.h"
#include <stdlib.h>
#include <string.h>

enum {
    CANDIDATE_COUNT = 12,
    CANDIDATE_STRIP_WIDTH = 399,
    ASCII_KEYBOARD_HEIGHT = 195,
    PINYIN_KEYBOARD_WIDTH = 472,
    PINYIN_ALPHABET_HEIGHT = 136,
    PINYIN_ACTION_HEIGHT = 38,
    PINYIN_ACTION_GAP = 10,
    PINYIN_BOTTOM = 6,
    PINYIN_DOCK_HEIGHT = 64 + PINYIN_ALPHABET_HEIGHT + PINYIN_ACTION_HEIGHT +
        PINYIN_ACTION_GAP + PINYIN_BOTTOM,
    MIN_GLIDE_DISTANCE = 15,
    SWIPE_MATCHES = 4,
    ALTERNATE_WORDS = 2,
    METEOR_POINTS = 17,
    METEOR_LIFETIME_MS = 180,
    METEOR_FRAME_MS = 16,
    METEOR_RADIUS = 8,
};
#define LETTER(width) (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (width))
#define GAP(width) (LV_BUTTONMATRIX_CTRL_HIDDEN | (width))

/* USER_1 is reserved for this layout. Standard LVGL maps are also used by Wi-Fi. */
static const char *const pinyin_map[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "_", "a", "s", "d", "f", "g", "h", "j", "k", "l", "_", "\n",
    "_", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "",
};

static const lv_buttonmatrix_ctrl_t pinyin_controls[] = {
    LETTER(1), LETTER(1), LETTER(1), LETTER(1), LETTER(1),
    LETTER(1), LETTER(1), LETTER(1), LETTER(1), LETTER(1),
    GAP(1), LETTER(2), LETTER(2), LETTER(2), LETTER(2),
    LETTER(2), LETTER(2), LETTER(2), LETTER(2), LETTER(2), GAP(1),
    GAP(3), LETTER(2), LETTER(2), LETTER(2), LETTER(2),
    LETTER(2), LETTER(2), LETTER(2), LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3,
};

static const char *const pinyin_action_map[] = {"1#", ",", " ", ".", LV_SYMBOL_OK, LV_SYMBOL_NEW_LINE, ""};
static const lv_buttonmatrix_ctrl_t pinyin_action_controls[] = {
    LETTER(3), LETTER(2), LETTER(8), LETTER(2), LETTER(3), LETTER(3),
};
#undef LETTER
#undef GAP

typedef struct {
    app_pinyin_swipe_point_t point;
    uint32_t stamp;
} meteor_sample_t;

typedef struct {
    lv_obj_t *dock, *keyboard, *actions, *textarea, *mode, *spelling, *strip, *next;
    lv_obj_t *buttons[CANDIDATE_COUNT], *labels[CANDIDATE_COUNT];
    lv_timer_t *timer, *meteor_timer;
    app_text_input_layout_t layout;
    void *layout_context;
    char composition[APP_PINYIN_MAX_SPELLING + 1];
    app_pinyin_candidate_t candidates[CANDIDATE_COUNT];
    app_pinyin_candidate_t alternate_words[SWIPE_MATCHES - 1][ALTERNATE_WORDS];
    char matches[SWIPE_MATCHES][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
    char alternatives[SWIPE_MATCHES - 1][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
    size_t alternative_count, alternative_prefix, candidate_offset, next_offset;
    app_pinyin_swipe_point_t centers[26], trace[APP_PINYIN_SWIPE_MAX_POINTS];
    uint32_t center_mask, trace_next_letters;
    size_t trace_count;
    meteor_sample_t meteor[METEOR_POINTS];
    size_t meteor_count;
    lv_area_t meteor_area;
    lv_indev_t *trace_input;
    char initial_letter;
    bool wanted, visible, pinyin, moved, pending_swipe, meteor_running, meteor_drawn;
} text_input_t;

static void cancel_trace(text_input_t *input);

static bool letter_key(const char *key)
{
    return key && key[0] >= 'a' && key[0] <= 'z' && !key[1];
}

static bool displayable(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;
    while (*bytes) {
        const unsigned lead = *bytes++;
        unsigned tail = lead < 0x80 ? 0 : lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
        uint32_t cp = tail ? lead & (0x7Fu >> (tail + 1)) : lead;
        while (tail--) {
            if ((*bytes & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (*bytes++ & 0x3F);
        }
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(app_ui_chinese_font(), &glyph, cp, 0) || glyph.is_placeholder)
            return false;
    }
    return true;
}

static void read_alternative(const text_input_t *input, const char *alternative,
                             app_pinyin_candidate_t words[ALTERNATE_WORDS])
{
    char spelling[sizeof(input->composition)];
    const size_t added = strlen(alternative);
    const size_t length = input->alternative_prefix + added;
    if (length >= sizeof(spelling)) return;
    memcpy(spelling, input->composition, input->alternative_prefix);
    memcpy(spelling + input->alternative_prefix, alternative, added + 1);
    const size_t count = app_pinyin_lookup(spelling, 0, words, ALTERNATE_WORDS, NULL);
    const size_t pending = strlen(input->composition);
    for (size_t i = 0; i < count; ++i)
        words[i].consumed = words[i].consumed == length && displayable(words[i].text) ? pending : 0;
}

static size_t add_alternatives(text_input_t *input, size_t count, bool *more)
{
    memset(input->alternate_words, 0, sizeof(input->alternate_words));
    for (size_t i = 0; i < input->alternative_count; ++i)
        read_alternative(input, input->alternatives[i], input->alternate_words[i]);

    const size_t pending = strlen(input->composition);
    size_t insert = 0;
    while (insert < count && insert < 2 && input->candidates[insert].consumed == pending &&
           displayable(input->candidates[insert].text)) ++insert;
    for (size_t rank = 0; rank < ALTERNATE_WORDS; ++rank) {
        for (size_t i = 0; i < input->alternative_count; ++i) {
            const app_pinyin_candidate_t *word = &input->alternate_words[i][rank];
            if (!word->consumed) continue;
            bool duplicate = false;
            for (size_t j = 0; j < count; ++j)
                if (!strcmp(input->candidates[j].text, word->text)) duplicate = true;
            if (duplicate) continue;
            if (count == CANDIDATE_COUNT) {
                --count;
                --input->next_offset;
                *more = true;
            }
            memmove(input->candidates + insert + 1, input->candidates + insert,
                    (count - insert) * sizeof(input->candidates[0]));
            input->candidates[insert++] = *word;
            ++count;
        }
    }
    return count;
}

static void refresh_candidates(text_input_t *input)
{
    if (!input->spelling) return;
    bool more = false;
    size_t count = app_pinyin_lookup(input->composition, input->candidate_offset,
                                    input->candidates, CANDIDATE_COUNT, &more);
    input->next_offset = input->candidate_offset + count;
    if (!input->candidate_offset && input->alternative_count)
        count = add_alternatives(input, count, &more);
    size_t shown = 0;
    for (size_t i = 0; i < count; ++i) {
        if (displayable(input->candidates[i].text)) input->candidates[shown++] = input->candidates[i];
    }
    app_ui_label_text(input->spelling, input->composition);
    app_ui_label_text(lv_obj_get_child(input->mode, 0), input->pinyin ? "中" : "EN");
    for (size_t i = 0; i < CANDIDATE_COUNT; ++i) {
        lv_obj_t *button = input->buttons[i];
        if (i < shown) {
            const char *text = input->candidates[i].text;
            lv_point_t size;
            lv_text_get_size(&size, text, app_ui_chinese_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const int32_t width = LV_CLAMP(54, size.x + 20, CANDIDATE_STRIP_WIDTH - 24);
            lv_obj_set_width(button, width);
            lv_obj_set_width(input->labels[i], width - 12);
            app_ui_label_text(input->labels[i], text);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
        } else {
            input->candidates[i] = (app_pinyin_candidate_t){0};
            lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!more) input->next_offset = 0;
    if (more || input->candidate_offset) lv_obj_remove_flag(input->next, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(input->next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_x(input->strip, 0, LV_ANIM_OFF);
}

static void composition_changed(text_input_t *input)
{
    input->candidate_offset = 0;
    refresh_candidates(input);
}

static void clear_composition(text_input_t *input)
{
    input->composition[0] = '\0';
    input->alternative_count = 0;
    input->pending_swipe = false;
    composition_changed(input);
}

static uint32_t character_count(const char *text)
{
    uint32_t count = 0;
    for (const unsigned char *byte = (const unsigned char *)text; *byte; ++byte)
        if ((*byte & 0xc0U) != 0x80U) ++count;
    return count;
}

static bool insert_composition_text(text_input_t *input, const char *text)
{
    const uint32_t limit = lv_textarea_get_max_length(input->textarea);
    if (limit) {
        const uint32_t used = character_count(lv_textarea_get_text(input->textarea));
        if (used > limit || character_count(text) > limit - used) {
            /* LVGL inserts one character at a time in bounded fields. Avoid
             * committing half a sentence and discarding the other half. */
            app_ui_label_text(input->spelling, "输入框空间不足");
            return false;
        }
    }
    lv_textarea_add_text(input->textarea, text);
    return true;
}

static void accept_candidate(text_input_t *input, const app_pinyin_candidate_t *candidate)
{
    const size_t length = strlen(input->composition);
    if (!candidate->consumed || candidate->consumed > length) return;
    if (!insert_composition_text(input, candidate->text)) return;
    memmove(input->composition, input->composition + candidate->consumed,
            length - candidate->consumed + 1);
    input->alternative_count = 0;
    input->pending_swipe = false;
    composition_changed(input);
}

static bool first_candidate(const text_input_t *input, app_pinyin_candidate_t *candidate)
{
    const size_t length = strlen(input->composition);
    if (input->pending_swipe && !input->candidate_offset && input->candidates[0].consumed == length) {
        *candidate = input->candidates[0];
        return true;
    }
    return app_pinyin_lookup(input->composition, 0, candidate, 1, NULL) && displayable(candidate->text);
}

static bool finish_composition(text_input_t *input)
{
    if (!input->composition[0]) return true;
    app_pinyin_candidate_t first;
    const size_t length = strlen(input->composition);
    const bool have_candidate = first_candidate(input, &first);
    if (have_candidate && first.consumed < length) {
        app_ui_label_text(input->spelling, "请先选择候选");
        return false;
    }
    if (!insert_composition_text(input, have_candidate ? first.text : input->composition)) return false;
    clear_composition(input);
    return true;
}

static bool append_spelling(text_input_t *input, const char *text)
{
    const size_t prefix = strlen(input->composition);
    const size_t added = strlen(text);
    if (added >= sizeof(input->composition) - prefix) {
        app_ui_label_text(input->spelling, "先选候选，再继续输入");
        return false;
    }
    memcpy(input->composition + prefix, text, added + 1);
    input->alternative_prefix = prefix;
    input->alternative_count = 0;
    input->pending_swipe = false;
    return true;
}

static void update_keyboard_layout(text_input_t *input)
{
    if (!input->actions) return;
    const bool alphabet = lv_keyboard_get_mode(input->keyboard) == LV_KEYBOARD_MODE_USER_1;
    const int32_t bottom = PINYIN_BOTTOM + (alphabet ? PINYIN_ACTION_HEIGHT + PINYIN_ACTION_GAP : 0);
    lv_obj_set_size(input->keyboard, PINYIN_KEYBOARD_WIDTH, alphabet ? PINYIN_ALPHABET_HEIGHT : 183);
    lv_obj_align(input->keyboard, LV_ALIGN_BOTTOM_MID, 0, -bottom);
    lv_obj_set_ext_click_area(input->keyboard, alphabet ? 5 : 0);
    if (alphabet) lv_obj_remove_flag(input->actions, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(input->actions, LV_OBJ_FLAG_HIDDEN);
}

static void candidate_event(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    if (button == input->mode) {
        cancel_trace(input);
        if (!finish_composition(input)) return;
        input->pinyin = !input->pinyin;
        lv_keyboard_set_mode(input->keyboard, input->pinyin ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_TEXT_LOWER);
        update_keyboard_layout(input);
        refresh_candidates(input);
    } else if (button == input->next) {
        input->candidate_offset = input->next_offset;
        refresh_candidates(input);
    } else {
        for (size_t i = 0; i < CANDIDATE_COUNT; ++i)
            if (button == input->buttons[i]) {
                accept_candidate(input, &input->candidates[i]);
                break;
            }
    }
}

static const char *selected_key(const lv_obj_t *matrix)
{
    const uint32_t index = lv_buttonmatrix_get_selected_button(matrix);
    return index == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(matrix, index);
}

static void keyboard_event(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    const char *key = selected_key(input->keyboard);
    if (!key) return;
    if (input->pinyin && letter_key(key)) {
        /* The buttonmatrix emits its own release event for taps. The pointer
         * handler owns both taps and gestures from the same alphabetic press. */
        if (!input->trace_input && append_spelling(input, key)) composition_changed(input);
        return;
    }
    const size_t length = strlen(input->composition);
    if (length && !strcmp(key, LV_SYMBOL_BACKSPACE)) {
        input->composition[input->pending_swipe ? input->alternative_prefix : length - 1] = '\0';
        input->alternative_count = 0;
        input->pending_swipe = false;
        composition_changed(input);
        return;
    }
    if (!finish_composition(input)) return;
    if (input->pinyin && !strcmp(key, "abc")) {
        lv_keyboard_set_mode(input->keyboard, LV_KEYBOARD_MODE_USER_1);
        update_keyboard_layout(input);
        return;
    }
    lv_keyboard_def_event_cb(event);
}

static void action_event(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    const char *key = selected_key(input->actions);
    if (!key || input->trace_input) return;
    if (input->composition[0] && !strcmp(key, " ")) {
        app_pinyin_candidate_t first;
        if (first_candidate(input, &first)) {
            accept_candidate(input, &first);
            return;
        }
    }
    if (!finish_composition(input)) return;
    if (!strcmp(key, "1#")) {
        lv_keyboard_set_mode(input->keyboard, LV_KEYBOARD_MODE_SPECIAL);
        update_keyboard_layout(input);
    } else if (!strcmp(key, LV_SYMBOL_NEW_LINE)) {
        if (lv_textarea_get_one_line(input->textarea)) lv_obj_send_event(input->textarea, LV_EVENT_READY, NULL);
        else lv_textarea_add_char(input->textarea, '\n');
    } else if (!strcmp(key, LV_SYMBOL_OK)) {
        if (lv_obj_send_event(input->keyboard, LV_EVENT_READY, NULL) == LV_RESULT_OK)
            lv_obj_send_event(input->textarea, LV_EVENT_READY, NULL);
    } else {
        lv_textarea_add_text(input->textarea, !strcmp(key, ",") ? "，" : !strcmp(key, ".") ? "。" : key);
    }
}

static void remember_center(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    if (lv_keyboard_get_mode(input->keyboard) != LV_KEYBOARD_MODE_USER_1) return;
    lv_draw_task_t *task = lv_event_get_draw_task(event);
    if (lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_FILL) return;
    const lv_draw_dsc_base_t *descriptor = lv_draw_task_get_draw_dsc(task);
    if (descriptor->part != LV_PART_ITEMS) return;
    const char *letter = lv_keyboard_get_button_text(input->keyboard, descriptor->id1);
    if (!letter_key(letter)) return;
    lv_area_t area;
    lv_draw_task_get_area(task, &area);
    const unsigned index = (unsigned)(letter[0] - 'a');
    input->centers[index] = (app_pinyin_swipe_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    input->center_mask |= 1UL << index;
}

static void include_area(lv_area_t *left, const lv_area_t *right)
{
    left->x1 = LV_MIN(left->x1, right->x1);
    left->y1 = LV_MIN(left->y1, right->y1);
    left->x2 = LV_MAX(left->x2, right->x2);
    left->y2 = LV_MAX(left->y2, right->y2);
}

static lv_area_t meteor_disc(app_pinyin_swipe_point_t point, int radius)
{
    return (lv_area_t){point.x - radius, point.y - radius, point.x + radius, point.y + radius};
}

static void invalidate_meteor(text_input_t *input)
{
    lv_area_t dirty = input->meteor_area;
    const bool had_trail = input->meteor_drawn;
    input->meteor_drawn = input->meteor_count > 0;
    if (input->meteor_drawn) {
        input->meteor_area = meteor_disc(input->meteor[0].point, METEOR_RADIUS);
        for (size_t i = 1; i < input->meteor_count; ++i) {
            const lv_area_t area = meteor_disc(input->meteor[i].point, METEOR_RADIUS);
            include_area(&input->meteor_area, &area);
        }
        if (had_trail) include_area(&dirty, &input->meteor_area);
        else dirty = input->meteor_area;
    }
    if (had_trail || input->meteor_drawn) lv_obj_invalidate_area(input->keyboard, &dirty);
}

static void meteor_tick(lv_timer_t *timer)
{
    text_input_t *input = lv_timer_get_user_data(timer);
    const uint32_t now = lv_tick_get();
    size_t old = 0;
    while (old < input->meteor_count && now - input->meteor[old].stamp >= METEOR_LIFETIME_MS) ++old;
    if (old) {
        input->meteor_count -= old;
        memmove(input->meteor, input->meteor + old, input->meteor_count * sizeof(input->meteor[0]));
    }
    invalidate_meteor(input);
    if (!input->meteor_count) {
        input->meteor_running = false;
        lv_timer_pause(timer);
    }
}

static void record_meteor(text_input_t *input, app_pinyin_swipe_point_t point)
{
    if (!input->meteor_timer) return;
    if (input->meteor_count == METEOR_POINTS) {
        memmove(input->meteor, input->meteor + 1, (METEOR_POINTS - 1) * sizeof(input->meteor[0]));
        --input->meteor_count;
    }
    input->meteor[input->meteor_count++] = (meteor_sample_t){point, lv_tick_get()};
    if (input->moved && !input->meteor_running) {
        input->meteor_running = true;
        lv_timer_resume(input->meteor_timer);
        lv_timer_ready(input->meteor_timer);
    }
}

static uint32_t meteor_opacity(const meteor_sample_t *sample, uint32_t now)
{
    const uint32_t age = now - sample->stamp;
    if (age >= METEOR_LIFETIME_MS) return 0;
    const uint32_t remaining = METEOR_LIFETIME_MS - age;
    return remaining * remaining * LV_OPA_COVER / (METEOR_LIFETIME_MS * METEOR_LIFETIME_MS);
}

static void draw_meteor(lv_event_t *event)
{
    const text_input_t *input = lv_event_get_user_data(event);
    if (!input->meteor_running || !input->meteor_count) return;
    lv_layer_t *layer = lv_event_get_layer(event);
    const uint32_t now = lv_tick_get();
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.round_start = line.round_end = 1;
    for (size_t i = 1; i < input->meteor_count; ++i) {
        const meteor_sample_t *from = &input->meteor[i - 1], *to = &input->meteor[i];
        const uint32_t along = (uint32_t)(i * 255U / (input->meteor_count - 1));
        const uint32_t intensity = meteor_opacity(to, now) * (80U + along) / 335U;
        if (!intensity) continue;
        line.p1 = (lv_point_precise_t){from->point.x, from->point.y};
        line.p2 = (lv_point_precise_t){to->point.x, to->point.y};
        line.width = 1 + (int32_t)(along * 4U / 255U);
        line.opa = (lv_opa_t)intensity;
        line.color = lv_color_mix(lv_color_hex(0xBBFFF5), lv_color_hex(0x498DDA), (uint8_t)along);
        lv_draw_line(layer, &line);
    }
    const meteor_sample_t *head = &input->meteor[input->meteor_count - 1];
    const uint32_t glow = meteor_opacity(head, now);
    if (!glow) return;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(0x6EF2D8);
    dot.bg_opa = (lv_opa_t)(glow / 3U);
    lv_area_t area = meteor_disc(head->point, 7);
    lv_draw_rect(layer, &dot, &area);
    dot.bg_color = lv_color_hex(0xECFFFB);
    dot.bg_opa = (lv_opa_t)glow;
    area = meteor_disc(head->point, 3);
    lv_draw_rect(layer, &dot, &area);
}

static void release_trace(text_input_t *input)
{
    input->trace_input = NULL;
    input->trace_count = 0;
    input->moved = false;
}

static void cancel_trace(text_input_t *input)
{
    release_trace(input);
    if (input->meteor_running) invalidate_meteor(input);
    input->meteor_count = 0;
    input->meteor_running = false;
    if (input->meteor_timer) lv_timer_pause(input->meteor_timer);
    invalidate_meteor(input);
}

static void sample_trace(text_input_t *input, app_pinyin_swipe_point_t point, bool final)
{
    const app_pinyin_swipe_point_t last = input->trace[input->trace_count - 1];
    const int dx = point.x - last.x, dy = point.y - last.y;
    if (dx == 0 && dy == 0) return;
    if (!final && dx * dx + dy * dy < 16) return;
    if (input->trace_count == APP_PINYIN_SWIPE_MAX_POINTS)
        input->trace_count = app_pinyin_swipe_compact(input->trace, input->trace_count);
    input->trace[input->trace_count++] = point;
    const int x = point.x - input->trace[0].x, y = point.y - input->trace[0].y;
    /* The decoder needs a span of half a key. Keep smaller drifts as taps
     * so a finger landing near a key edge does not enter a dead zone. */
    const int spacing = input->centers['s' - 'a'].x - input->centers[0].x;
    const int trigger = LV_MAX(MIN_GLIDE_DISTANCE, spacing / 2);
    if (x * x + y * y >= trigger * trigger) input->moved = true;
    record_meteor(input, point);
}

static int key_distance(app_pinyin_swipe_point_t point, app_pinyin_swipe_point_t center)
{
    const int x = point.x - center.x, y = point.y - center.y;
    return x * x + y * y;
}

static char initial_letter(text_input_t *input, app_pinyin_swipe_point_t sample)
{
    const char *key = selected_key(input->keyboard);
    if (key && !letter_key(key)) return '\0';

    /* A start between two letter caps can still begin the stroke. Explicit
     * symbol, space and action keys keep their original hit targets. */
    int best = 29 * 29;
    char letter = key ? key[0] : '\0';
    if (!letter) {
        for (unsigned i = 0; i < 26; ++i) {
            if (!(input->center_mask & (1UL << i))) continue;
            const int distance = key_distance(sample, input->centers[i]);
            if (distance < best) {
                best = distance;
                letter = (char)('a' + i);
            }
        }
    }
    if (!letter) return '\0';

    input->trace_next_letters = app_pinyin_next_letters(input->composition);
    const unsigned index = letter - 'a';
    if (!input->trace_next_letters || !(input->center_mask & (1UL << index)) ||
        (input->trace_next_letters & (1UL << index))) return letter;

    /* Shift a boundary by at most about a tenth of a key's pitch. A tap on
     * the center of any key remains intentional, even with an invalid prefix. */
    const int pitch = input->centers['s' - 'a'].x - input->centers[0].x;
    if (pitch <= 0) return letter;
    best = key_distance(sample, input->centers[index]) + pitch * pitch / 5;
    const uint32_t eligible = input->trace_next_letters & input->center_mask;
    for (unsigned i = 0; i < 26; ++i) {
        if (!(eligible & (1UL << i))) continue;
        const int distance = key_distance(sample, input->centers[i]);
        if (distance < best) {
            best = distance;
            letter = (char)('a' + i);
        }
    }
    return letter;
}

static int trace_segment_length(app_pinyin_swipe_point_t a, app_pinyin_swipe_point_t b)
{
    const int x = abs(a.x - b.x), y = abs(a.y - b.y);
    return x > y ? x + y / 2 : y + x / 2;
}

static bool trace_point_inside(app_pinyin_swipe_point_t point, const lv_area_t *area)
{
    return point.x >= area->x1 && point.x <= area->x2 &&
           point.y >= area->y1 && point.y <= area->y2;
}

static size_t trim_edge_excursions(text_input_t *input)
{
    const int spacing = input->centers['s' - 'a'].x - input->centers[0].x;
    if (spacing <= 0) return input->trace_count;
    lv_area_t bounds = {input->centers[0].x, input->centers[0].y,
                        input->centers[0].x, input->centers[0].y};
    for (unsigned i = 1; i < 26; ++i) {
        const app_pinyin_swipe_point_t key = input->centers[i];
        const lv_area_t center = {key.x, key.y, key.x, key.y};
        include_area(&bounds, &center);
    }
    lv_area_increase(&bounds, spacing, spacing);
    lv_area_t tolerated = bounds;
    lv_area_increase(&tolerated, spacing / 2, spacing / 2);
    /* The letter surface retains the pointer even across the action bar.
     * Ignore a brief excursion without treating an unrelated path as typing. */
    size_t kept = 0;
    int excursion = 0, outside = 0;
    app_pinyin_swipe_point_t previous = input->trace[0];
    for (size_t i = 0; i < input->trace_count; ++i) {
        const app_pinyin_swipe_point_t point = input->trace[i];
        const bool inside = trace_point_inside(point, &bounds);
        if (!inside || excursion) {
            if (!kept || !trace_point_inside(point, &tolerated)) return 0;
            const int length = trace_segment_length(previous, point);
            excursion += length;
            outside += length;
            if (excursion > 3 * spacing || outside > 5 * spacing) return 0;
        }
        if (inside) {
            excursion = 0;
            input->trace[kept++] = point;
        }
        previous = point;
    }
    return kept;
}

static void keyboard_pointer(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        const lv_indev_t *device = lv_event_get_param(event);
        if (!device || input->trace_input == device) cancel_trace(input);
        return;
    }
    lv_indev_t *device = lv_event_get_indev(event);
    if (!device || lv_indev_get_type(device) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(device, &point);
    const app_pinyin_swipe_point_t sample = {point.x, point.y};
    if (code == LV_EVENT_PRESSED) {
        cancel_trace(input);
        if (input->pinyin && lv_keyboard_get_mode(input->keyboard) == LV_KEYBOARD_MODE_USER_1) {
            const char letter = initial_letter(input, sample);
            if (!letter) return;
            if (!input->composition[0]) app_ui_label_text(input->spelling, "");
            input->trace_input = device;
            input->initial_letter = letter;
            input->trace[0] = sample;
            input->trace_count = 1;
            record_meteor(input, sample);
        }
        return;
    }
    if (input->trace_input != device) return;
    const bool released = code == LV_EVENT_RELEASED;
    sample_trace(input, sample, released);
    if (!released) return;
    if (!input->moved) {
        const char tap[] = {input->initial_letter, '\0'};
        if (append_spelling(input, tap)) composition_changed(input);
    } else if (input->center_mask == ((1UL << 26) - 1)) {
        const size_t points = trim_edge_excursions(input);
        const size_t count = app_pinyin_swipe_decode_context(input->trace, points, input->centers,
                                                            input->trace_next_letters, input->matches, SWIPE_MATCHES);
        if (count && append_spelling(input, input->matches[0])) {
            input->pending_swipe = true;
            input->alternative_count = count - 1;
            memcpy(input->alternatives, input->matches + 1,
                   input->alternative_count * sizeof(input->matches[0]));
            composition_changed(input);
        } else if (!count) app_ui_label_text(input->spelling,
            input->trace_count >= 28 ? "未识别，可分段滑行" : "再试一次滑行");
    }
    release_trace(input);
}

static void update_visibility(text_input_t *input)
{
    aether_usb_host_status_t status;
    const bool external_keyboard = app_otg_get_status(&status) == ESP_OK &&
        status.active && status.keyboard_interfaces;
    const bool visible = input->wanted && !external_keyboard;
    if (visible == input->visible) return;
    input->visible = visible;
    if (visible) lv_obj_remove_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_add_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
        cancel_trace(input);
        clear_composition(input);
    }
    if (input->layout) input->layout(input->layout_context, visible);
}

static void update_timer(lv_timer_t *timer)
{
    update_visibility(lv_timer_get_user_data(timer));
}

static void dock_deleted(lv_event_t *event)
{
    text_input_t *input = lv_event_get_user_data(event);
    if (input->timer) lv_timer_delete(input->timer);
    if (input->meteor_timer) lv_timer_delete(input->meteor_timer);
    /* The child keyboard can receive a final pointer reset during its own deletion. */
    if (input->mode) lv_obj_remove_event_cb(input->keyboard, keyboard_pointer);
    free(input);
}

static lv_obj_t *candidate_button(text_input_t *input, lv_obj_t *parent, const char *text,
                                  int32_t width, lv_obj_t **label)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 38);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(button);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x253D45), 0);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_add_event_cb(button, candidate_event, LV_EVENT_CLICKED, input);
    lv_obj_t *title = app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0,
                                      0xE9F6F4, app_ui_chinese_font());
    if (label) *label = title;
    return button;
}

static void setup_pinyin(text_input_t *input)
{
    lv_obj_t *keyboard = input->keyboard;
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(keyboard, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x263A40), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x32665F), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xE9F6F4), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, pinyin_map, pinyin_controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_VALUE_CHANGED, input);
    lv_obj_add_event_cb(keyboard, remember_center, LV_EVENT_DRAW_TASK_ADDED, input);
    lv_obj_add_event_cb(keyboard, draw_meteor, LV_EVENT_DRAW_POST, input);
    input->meteor_timer = lv_timer_create(meteor_tick, METEOR_FRAME_MS, input);
    if (input->meteor_timer) lv_timer_pause(input->meteor_timer);
    const lv_event_code_t events[] = {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                      LV_EVENT_PRESS_LOST, LV_EVENT_INDEV_RESET};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i)
        lv_obj_add_event_cb(keyboard, keyboard_pointer, events[i], input);

    input->actions = lv_buttonmatrix_create(input->dock);
    lv_obj_set_size(input->actions, 446, PINYIN_ACTION_HEIGHT);
    lv_obj_align(input->actions, LV_ALIGN_BOTTOM_MID, 0, -PINYIN_BOTTOM);
    lv_obj_set_style_bg_opa(input->actions, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input->actions, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(input->actions, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(input->actions, 7, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(input->actions, lv_color_hex(0x203339), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(input->actions, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(input->actions, lv_color_hex(0x32665F), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(input->actions, lv_color_hex(0xE9F6F4), LV_PART_ITEMS);
    lv_obj_set_style_text_font(input->actions, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_add_flag(input->actions, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(input->actions, LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_group_remove_obj(input->actions);
    lv_buttonmatrix_set_map(input->actions, pinyin_action_map);
    lv_buttonmatrix_set_ctrl_map(input->actions, pinyin_action_controls);
    lv_obj_add_event_cb(input->actions, action_event, LV_EVENT_VALUE_CHANGED, input);
    update_keyboard_layout(input);

    input->mode = candidate_button(input, input->dock, "中", 48, NULL);
    lv_obj_set_pos(input->mode, 18, 4);
    input->spelling = app_ui_add_text(input->dock, "", LV_ALIGN_TOP_LEFT, 77, 43,
                                      0x9CD4CD, app_ui_chinese_font());
    lv_obj_set_width(input->spelling, 365);
    const int32_t label_height = lv_font_get_line_height(app_ui_chinese_font());
    lv_obj_set_height(input->spelling, label_height);
    lv_label_set_long_mode(input->spelling, LV_LABEL_LONG_DOT);
    input->strip = lv_obj_create(input->dock);
    lv_obj_set_pos(input->strip, 77, 4);
    lv_obj_set_size(input->strip, CANDIDATE_STRIP_WIDTH, 40);
    lv_obj_set_style_bg_opa(input->strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input->strip, 0, 0);
    lv_obj_set_style_pad_all(input->strip, 0, 0);
    lv_obj_set_style_pad_column(input->strip, 6, 0);
    lv_obj_set_scrollbar_mode(input->strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(input->strip, LV_DIR_HOR);
    lv_obj_remove_flag(input->strip, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_flex_flow(input->strip, LV_FLEX_FLOW_ROW);
    for (size_t i = 0; i < CANDIDATE_COUNT; ++i) {
        input->buttons[i] = candidate_button(input, input->strip, "", 54, &input->labels[i]);
        lv_obj_set_height(input->labels[i], label_height);
        lv_label_set_long_mode(input->labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(input->labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    input->next = candidate_button(input, input->strip, LV_SYMBOL_RIGHT, 48, NULL);
    refresh_candidates(input);
}

lv_obj_t *app_text_input_create(lv_obj_t *parent, lv_obj_t *textarea, bool pinyin,
                                app_text_input_layout_t layout, void *context)
{
    text_input_t *input = calloc(1, sizeof(*input));
    if (!input) return NULL;
    input->dock = lv_obj_create(parent);
    input->textarea = textarea;
    input->layout = layout;
    input->layout_context = context;
    input->pinyin = pinyin;
    lv_obj_set_user_data(input->dock, input);
    lv_obj_set_size(input->dock, 480, pinyin ? PINYIN_DOCK_HEIGHT : ASCII_KEYBOARD_HEIGHT);
    lv_obj_align(input->dock, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(input->dock, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(input->dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(input->dock, 0, 0);
    lv_obj_set_style_radius(input->dock, 0, 0);
    lv_obj_set_style_border_width(input->dock, 0, 0);
    lv_obj_set_style_bg_color(input->dock, lv_color_hex(pinyin ? 0x14272C : 0x18243A), 0);
    lv_obj_add_event_cb(input->dock, dock_deleted, LV_EVENT_DELETE, input);

    input->keyboard = lv_keyboard_create(input->dock);
    lv_obj_set_size(input->keyboard, 480, ASCII_KEYBOARD_HEIGHT);
    lv_obj_set_style_text_font(input->keyboard, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_group_remove_obj(input->keyboard);
    lv_keyboard_set_textarea(input->keyboard, textarea);
    if (pinyin) setup_pinyin(input);
    input->timer = lv_timer_create(update_timer, 160, input);
    return input->dock;
}

void app_text_input_show(lv_obj_t *dock, bool show)
{
    if (!dock) return;
    text_input_t *input = lv_obj_get_user_data(dock);
    input->wanted = show;
    update_visibility(input);
}
