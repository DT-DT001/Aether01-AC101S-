#include "app_ui_internal.h"
#include "app_calc_modes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CALC_BACKSPACE = 128, CALC_SIGN, CALC_CLEAR_ENTRY, CALC_SQUARE, CALC_ROOT, CALC_INVERSE,
    CALC_STANDARD, CALC_SCIENTIFIC, CALC_PROGRAMMER, CALC_ANGLE, CALC_SECOND,
    CALC_PI_KEY, CALC_E_KEY, CALC_PROGRAM_CLEAR, CALC_WORD, CALC_RADIX_BIN, CALC_RADIX_OCT,
    CALC_RADIX_DEC, CALC_RADIX_HEX, CALC_SCI_FUNCTION = 0x200,
};

typedef struct {
    uint64_t value, left;
    unsigned radix, bits, operation;
    bool entering, error, full;
} programmer_t;

typedef struct {
    char entry[32];
    char expression[80];
    double left, repeat_right;
    char operation, repeat_operation;
    bool entering, replace_entry, error;
    unsigned mode;
    bool degrees, second;
    programmer_t programmer;
    lv_obj_t *result, *formula, *readout, *keypad, *radix_row;
    lv_obj_t *tabs[3], *radices[4], *word_label, *angle_label, *second_key;
    lv_obj_t *functions[8], *digit_keys[16];
} calculator_t;

static void reset_entry(calculator_t *calc)
{
    strcpy(calc->entry, "0");
    calc->entering = false;
    calc->replace_entry = false;
    calc->error = false;
}

static void clear_calculation(calculator_t *calc)
{
    reset_entry(calc);
    calc->operation = calc->repeat_operation = 0;
    calc->expression[0] = '\0';
}

static double entry_value(const calculator_t *calc)
{
    return strtod(calc->entry, NULL);
}

static bool set_value(calculator_t *calc, double value)
{
    calc->error = !isfinite(value);
    if (calc->error) {
        calc->operation = calc->repeat_operation = 0;
        strcpy(calc->entry, "Error");
        return false;
    }
    snprintf(calc->entry, sizeof(calc->entry), "%.12g", value == 0 ? 0.0 : value);
    return true;
}

static double calculate(double left, char operation, double right)
{
    switch (operation) {
    case '+': return left + right;
    case '-': return left - right;
    case '*': return left * right;
    case '/': return right == 0 ? NAN : left / right;
    case '^': {
        double result;
        return app_calc_power(left, right, &result) ? result : NAN;
    }
    default: return right;
    }
}

static const char *operation_text(char operation)
{
    switch (operation) {
    case '*': return "×";
    case '/': return "/";
    case '+': return "+";
    case '^': return "^";
    default: return "-";
    }
}

static void enter_character(calculator_t *calc, char character)
{
    if (!calc->entering || calc->replace_entry) {
        if (!calc->operation) calc->expression[0] = '\0';
        calc->repeat_operation = 0;
        reset_entry(calc);
        calc->entering = true;
    }
    char *number = calc->entry + (calc->entry[0] == '-');
    size_t length = strlen(number);
    if (character == '.') {
        if (strchr(number, '.') || strchr(number, 'e')) return;
    } else if (length == 1 && *number == '0') {
        *number = character;
        return;
    } else if (length >= (size_t)(12 + (strchr(number, '.') != NULL)) || strchr(number, 'e')) {
        return;
    }
    number[length] = character;
    number[length + 1] = '\0';
}

static void enter_operation(calculator_t *calc, char key)
{
    const double value = entry_value(calc);
    if (key == '=') {
        const char operation = calc->operation ? calc->operation : calc->repeat_operation;
        if (!operation) return;
        const double left = calc->operation ? calc->left : value;
        const double right = !calc->operation ? calc->repeat_right :
                             calc->entering ? value : left;
        snprintf(calc->expression, sizeof(calc->expression), "%.12g %s %.12g =",
                 left, operation_text(operation), right);
        calc->operation = 0;
        if (set_value(calc, calculate(left, operation, right))) {
            calc->repeat_operation = operation;
            calc->repeat_right = right;
        }
    } else {
        if (calc->operation && calc->entering &&
            !set_value(calc, calculate(calc->left, calc->operation, value))) return;
        calc->operation = key;
        calc->repeat_operation = 0;
        calc->left = entry_value(calc);
        snprintf(calc->expression, sizeof(calc->expression), "%s %s", calc->entry, operation_text(key));
    }
    calc->entering = false;
}

static void enter_unary(calculator_t *calc, unsigned key)
{
    const double value = entry_value(calc);
    double result = value;
    switch (key) {
    case '%':
        result = value / 100;
        if (calc->operation == '+' || calc->operation == '-') result *= calc->left;
        break;
    case CALC_SQUARE: result = value * value; break;
    case CALC_ROOT: result = value < 0 ? NAN : sqrt(value); break;
    case CALC_INVERSE: result = value == 0 ? NAN : 1 / value; break;
    case CALC_PI_KEY: result = APP_CALC_PI; break;
    case CALC_E_KEY: result = APP_CALC_E; break;
    default:
        if (key >= CALC_SCI_FUNCTION &&
            !app_calc_scientific((app_calc_scientific_op_t)(key - CALC_SCI_FUNCTION),
                                 value, calc->degrees, &result)) result = NAN;
        break;
    }
    if (!calc->operation) calc->expression[0] = '\0';
    calc->repeat_operation = 0;
    const bool constant = key == CALC_PI_KEY || key == CALC_E_KEY;
    calc->entering = set_value(calc, result) && (calc->operation || constant);
    calc->replace_entry = true;
}

static void calculator_press(calculator_t *calc, unsigned key)
{
    const bool character = (key >= '0' && key <= '9') || key == '.';
    const bool constant = key == CALC_PI_KEY || key == CALC_E_KEY;
    if (calc->error && !character && !constant && key != 'C' && key != CALC_CLEAR_ENTRY) return;
    if (calc->error && constant) clear_calculation(calc);
    if (character) {
        if (calc->error) clear_calculation(calc);
        enter_character(calc, (char)key);
    } else if (key == 'C') {
        clear_calculation(calc);
    } else if (key == CALC_CLEAR_ENTRY) {
        reset_entry(calc);
        calc->repeat_operation = 0;
        if (!calc->operation) calc->expression[0] = '\0';
        calc->entering = true;
    } else if (key == CALC_SIGN) {
        if (!calc->entering && calc->operation) reset_entry(calc);
        const size_t length = strlen(calc->entry);
        if (calc->entry[0] == '-') memmove(calc->entry, calc->entry + 1, length);
        else {
            memmove(calc->entry + 1, calc->entry, length + 1);
            calc->entry[0] = '-';
        }
        if (calc->repeat_operation) calc->expression[0] = '\0';
        calc->repeat_operation = 0;
        calc->entering = true;
    } else if (key == CALC_BACKSPACE) {
        if (!calc->entering) return;
        const size_t length = strlen(calc->entry);
        if (length <= (size_t)(1 + (calc->entry[0] == '-')) || strchr(calc->entry, 'e')) reset_entry(calc);
        else calc->entry[length - 1] = '\0';
        calc->entering = true;
    } else if (key == '=' || key == '+' || key == '-' || key == '*' || key == '/' || key == '^') {
        enter_operation(calc, (char)key);
    } else {
        enter_unary(calc, key);
    }
}

static const unsigned radix_values[] = {2, 8, 10, 16};
static const char *const radix_labels[] = {"BIN", "OCT", "DEC", "HEX"};
static const unsigned word_sizes[] = {8, 16, 32, 64};

static bool calc_multiply_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                                uint32_t codepoint, uint32_t next)
{
    (void)next;
    /* These fonts ship the cross icon at U+F00D but omit U+00D7. */
    return codepoint == 0x00D7 &&
        lv_font_montserrat_16.get_glyph_dsc(font, glyph, 0xF00D, 0);
}

static const lv_font_t *math_font(bool large)
{
    static lv_font_t fonts[2];
    static bool ready;
    if (!ready) {
        fonts[0] = lv_font_montserrat_16;
        fonts[1] = lv_font_montserrat_24;
        for (unsigned i = 0; i < 2; ++i) {
            fonts[i].get_glyph_dsc = calc_multiply_glyph;
            fonts[i].fallback = app_ui_chinese_font_size(i ? 24 : 16);
        }
        ready = true;
    }
    return &fonts[large];
}

static int hex_digit(unsigned key)
{
    if (key >= '0' && key <= '9') return (int)(key - '0');
    if (key >= 'A' && key <= 'F') return (int)(key - 'A') + 10;
    if (key >= 'a' && key <= 'f') return (int)(key - 'a') + 10;
    return -1;
}

static const char *program_operator(unsigned key)
{
    switch (key) {
    case '&': return "AND";
    case '|': return "OR";
    case '^': return "XOR";
    case '<': return "<<";
    case '>': return ">>";
    case '%': return "MOD";
    default: return operation_text((char)key);
    }
}

static bool program_apply(programmer_t *p, unsigned key, uint64_t left)
{
    app_calc_program_op_t op;
    switch (key) {
    case '+': op = APP_CALC_ADD; break;
    case '-': op = APP_CALC_SUBTRACT; break;
    case '*': op = APP_CALC_MULTIPLY; break;
    case '/': op = APP_CALC_DIVIDE; break;
    case '%': op = APP_CALC_MOD; break;
    case '&': op = APP_CALC_AND; break;
    case '|': op = APP_CALC_OR; break;
    case '^': op = APP_CALC_XOR; break;
    case '~': op = APP_CALC_NOT; break;
    case '<': op = APP_CALC_SHL; break;
    case '>': op = APP_CALC_SHR; break;
    default: return false;
    }
    p->error = !app_calc_programmer_apply(op, left, p->value, p->bits, &p->value);
    if (p->error) p->operation = 0;
    return !p->error;
}

static void programmer_press(programmer_t *p, unsigned key)
{
    const int digit = hex_digit(key);
    p->full = false;
    if (key >= CALC_RADIX_BIN && key <= CALC_RADIX_HEX) {
        p->radix = radix_values[key - CALC_RADIX_BIN];
    } else if (key == CALC_WORD) {
        for (unsigned i = 0; i < sizeof(word_sizes) / sizeof(word_sizes[0]); ++i) {
            if (p->bits != word_sizes[i]) continue;
            p->bits = word_sizes[(i + 1) % (sizeof(word_sizes) / sizeof(word_sizes[0]))];
            break;
        }
        p->value &= app_calc_programmer_mask(p->bits);
        p->left &= app_calc_programmer_mask(p->bits);
    } else if (key == CALC_PROGRAM_CLEAR || key == CALC_CLEAR_ENTRY) {
        if (key == CALC_PROGRAM_CLEAR) p->operation = 0;
        p->value = 0;
        p->error = false;
        p->entering = true;
    } else if (digit >= 0 && (unsigned)digit < p->radix) {
        const uint64_t previous = p->entering && !p->error ? p->value : 0;
        p->full = previous > (app_calc_programmer_mask(p->bits) - (unsigned)digit) / p->radix;
        if (!p->full) {
            p->value = previous * p->radix + (unsigned)digit;
            p->entering = true;
            p->error = false;
        }
    } else if (!p->error && key == CALC_BACKSPACE && p->entering) {
        p->value /= p->radix;
    } else if (!p->error && key == '~') {
        (void)program_apply(p, key, p->value);
        p->entering = p->operation != 0;
    } else if (!p->error && key == '=') {
        if (p->operation) (void)program_apply(p, p->operation, p->left);
        p->operation = 0;
        p->entering = false;
    } else if (!p->error && key <= 127 && key && strchr("+-*/%&|^<>", (int)key)) {
        if (p->operation && p->entering && !program_apply(p, p->operation, p->left)) return;
        p->left = p->value;
        p->operation = key;
        p->entering = false;
    }
}

typedef struct { unsigned id; const char *text; } calc_key_t;

static const calc_key_t standard_keys[] = {
    {'%', "%"}, {CALC_CLEAR_ENTRY, "CE"}, {'C', "C"}, {CALC_BACKSPACE, LV_SYMBOL_BACKSPACE},
    {CALC_INVERSE, "1/x"}, {CALC_SQUARE, "x^2"}, {CALC_ROOT, "sqrt"}, {'/', "/"},
    {'7', "7"}, {'8', "8"}, {'9', "9"}, {'*', "×"},
    {'4', "4"}, {'5', "5"}, {'6', "6"}, {'-', "-"},
    {'1', "1"}, {'2', "2"}, {'3', "3"}, {'+', "+"},
    {CALC_SIGN, "+/-"}, {'0', "0"}, {'.', "."}, {'=', "="},
};

/* The first eight keys after DEG and 2nd have alternate functions. */
#define SCI(op, label) {CALC_SCI_FUNCTION + APP_CALC_##op, label}
static const calc_key_t scientific_keys[] = {
    {CALC_ANGLE, "DEG"}, {CALC_SECOND, "2nd"}, SCI(SIN, "sin"), SCI(COS, "cos"), SCI(TAN, "tan"),
    SCI(LOG10, "log"), SCI(LN, "ln"), {'^', "x^y"}, SCI(SQRT, "sqrt"), SCI(SQUARE, "x^2"),
    {'C', "C"}, SCI(FACTORIAL, "n!"), {CALC_PI_KEY, "pi"}, {CALC_E_KEY, "e"}, {CALC_BACKSPACE, LV_SYMBOL_BACKSPACE},
    {'7', "7"}, {'8', "8"}, {'9', "9"}, {'/', "/"}, {'%', "%"},
    {'4', "4"}, {'5', "5"}, {'6', "6"}, {'*', "×"}, {CALC_INVERSE, "1/x"},
    {'1', "1"}, {'2', "2"}, {'3', "3"}, {'-', "-"}, {CALC_CLEAR_ENTRY, "CE"},
    {CALC_SIGN, "+/-"}, {'0', "0"}, {'.', "."}, {'+', "+"}, {'=', "="},
};
static const calc_key_t scientific_alternates[] = {
    SCI(ASIN, "asin"), SCI(ACOS, "acos"), SCI(ATAN, "atan"), SCI(POW10, "10^x"),
    SCI(EXP, "e^x"), SCI(ABS, "|x|"), SCI(CBRT, "cbrt"), SCI(CUBE, "x^3"),
};
#undef SCI

static const calc_key_t programmer_keys[] = {
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"},
    {'7', "7"}, {'8', "8"}, {'9', "9"}, {'/', "/"}, {'%', "MOD"}, {CALC_BACKSPACE, LV_SYMBOL_BACKSPACE},
    {'4', "4"}, {'5', "5"}, {'6', "6"}, {'*', "×"}, {'&', "AND"}, {'|', "OR"},
    {'1', "1"}, {'2', "2"}, {'3', "3"}, {'-', "-"}, {'^', "XOR"}, {'~', "NOT"},
    {'0', "0"}, {'+', "+"}, {'=', "="}, {'<', "<<"}, {'>', ">>"}, {CALC_PROGRAM_CLEAR, "AC"},
};

static void refresh_calculator(calculator_t *calc)
{
    const lv_font_t *large = &lv_font_montserrat_48;
    const lv_font_t *medium = &lv_font_montserrat_28;
    const lv_font_t *font;
    const char *formula = calc->expression, *value = calc->entry;
    bool error = calc->error;
    char formatted[APP_CALC_PROGRAM_TEXT_SIZE + 1], operation[APP_CALC_PROGRAM_TEXT_SIZE + 10];
    if (calc->mode == CALC_PROGRAMMER) {
        const programmer_t *p = &calc->programmer;
        char left[APP_CALC_PROGRAM_TEXT_SIZE];
        app_calc_programmer_format(p->value, p->radix, p->bits, formatted);
        const size_t length = strlen(formatted);
        font = length <= 11 ? large : length <= 18 ? medium : math_font(true);
        if (p->radix == 2 && length > 32) {
            const size_t split = length - 32;
            memmove(formatted + split + 1, formatted + split, length - split + 1);
            formatted[split] = '\n';
        }
        if (p->radix == 2 && length > 22) font = math_font(false);
        value = formatted;
        error = p->error;
        formula = p->full ? "已达位宽上限" : "";
        if (p->operation && !p->full) {
            app_calc_programmer_format(p->left, p->radix, p->bits, left);
            snprintf(operation, sizeof(operation), "%s %s", strlen(left) > 32 ? "…" : left,
                     program_operator(p->operation));
            formula = operation;
        }
        for (unsigned i = 0; i < 4; ++i)
            lv_obj_set_state(calc->radices[i], LV_STATE_CHECKED, p->radix == radix_values[i]);
        for (unsigned i = 0; i < 16; ++i)
            lv_obj_set_state(calc->digit_keys[i], LV_STATE_DISABLED, i >= p->radix);
        app_ui_label_fmt(calc->word_label, "%u 位", p->bits);
    } else {
        font = strlen(value) > 11 ? medium : large;
    }
    lv_obj_set_style_text_font(calc->result, error ? math_font(true) : font, 0);
    lv_obj_set_style_text_color(calc->result, lv_color_hex(error ? 0xFF9C85 : 0xF2F7FF), 0);
    app_ui_label_text(calc->result, error ? "计算错误" : value);
    app_ui_label_text(calc->formula, formula);
}

static void draw_keypad(app_ui_t *ui);

static void press_key(app_ui_t *ui, unsigned key)
{
    calculator_t *calc = ui->page_context;
    if (key >= CALC_STANDARD && key <= CALC_PROGRAMMER) {
        if (calc->mode == key) return;
        calc->mode = key;
        draw_keypad(ui);
    } else if (calc->mode == CALC_PROGRAMMER) {
        programmer_press(&calc->programmer, key);
    } else if (key == CALC_ANGLE && calc->mode == CALC_SCIENTIFIC) {
        calc->degrees = !calc->degrees;
        app_ui_label_text(calc->angle_label, calc->degrees ? "DEG" : "RAD");
    } else if (key == CALC_SECOND && calc->mode == CALC_SCIENTIFIC) {
        calc->second = !calc->second;
        lv_obj_set_state(calc->second_key, LV_STATE_CHECKED, calc->second);
        for (unsigned i = 0; i < sizeof(scientific_alternates) / sizeof(scientific_alternates[0]); ++i) {
            lv_obj_t *button = calc->functions[i];
            const calc_key_t def = calc->second ? scientific_alternates[i] : scientific_keys[2 + i];
            lv_obj_set_user_data(button, (void *)(uintptr_t)def.id);
            app_ui_label_text(lv_obj_get_child(button, 0), def.text);
        }
    } else {
        calculator_press(calc, key);
    }
    refresh_calculator(calc);
}

static void calc_button(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    calculator_t *calc = ui->page_context;
    if (!calc) return;
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    press_key(ui, (unsigned)(uintptr_t)lv_obj_get_user_data(button));
}

static void calc_keyboard(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    calculator_t *calc = ui->page_context;
    if (!calc) return;
    unsigned key = lv_event_get_key(event);
    const bool programmer = calc->mode == CALC_PROGRAMMER;
    if (key == LV_KEY_BACKSPACE) key = CALC_BACKSPACE;
    else if (key == LV_KEY_DEL) key = CALC_CLEAR_ENTRY;
    else if (key == LV_KEY_ESC) key = programmer ? CALC_PROGRAM_CLEAR : 'C';
    else if (key == LV_KEY_ENTER && lv_event_get_current_target_obj(event) == calc->readout) key = '=';
    else if (key == 'x' || key == 'X') key = '*';
    else if (programmer) {
        if (hex_digit(key) < 0 && (key > 127 || !key || !strchr("=+-*/%&|^~<>", (int)key))) return;
    } else {
        if (key == 'c') key = 'C';
        if (key > 127 || !key || !strchr("0123456789=.+-*/%C^", (int)key) ||
            (key == '^' && calc->mode != CALC_SCIENTIFIC)) return;
    }
    press_key(ui, key);
}

static lv_obj_t *make_key(app_ui_t *ui, lv_obj_t *parent, calc_key_t key, int x, int y,
                          int width, int height, uint32_t fill, const lv_font_t *font)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x49688E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x296B76), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x142138), LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_user_data(button, (void *)(uintptr_t)key.id);
    lv_obj_add_event_cb(button, calc_button, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(button, calc_keyboard, LV_EVENT_KEY, ui);
    app_ui_add_text(button, key.text, LV_ALIGN_CENTER, 0, 0, key.id == '=' ? 0x0E2631 : 0xECF3FF, font);
    return button;
}

static void draw_keypad(app_ui_t *ui)
{
    calculator_t *calc = ui->page_context;
    const bool programmer = calc->mode == CALC_PROGRAMMER;
    const bool science = calc->mode == CALC_SCIENTIFIC;
    lv_obj_clean(calc->keypad);
    lv_obj_set_flag(calc->radix_row, LV_OBJ_FLAG_HIDDEN, !programmer);
    for (unsigned i = 0; i < 3; ++i)
        lv_obj_set_state(calc->tabs[i], LV_STATE_CHECKED, calc->mode == CALC_STANDARD + i);
    const calc_key_t *keys = programmer ? programmer_keys : science ? scientific_keys : standard_keys;
    const unsigned count = programmer ? sizeof(programmer_keys) / sizeof(*keys) :
        science ? sizeof(scientific_keys) / sizeof(*keys) : sizeof(standard_keys) / sizeof(*keys);
    const int columns = programmer ? 6 : science ? 5 : 4;
    const int step = science ? 35 : programmer ? 41 : 42;
    lv_obj_set_y(calc->keypad, programmer ? 159 : 117);
    for (unsigned i = 0; i < count; ++i) {
        calc_key_t key = keys[i];
        const bool alternate = science && i >= 2 && i < 10;
        if (alternate && calc->second) key = scientific_alternates[i - 2];
        if (key.id == CALC_ANGLE && !calc->degrees) key.text = "RAD";
        const int col = (int)i % columns, row = (int)i / columns;
        const int x = col * 412 / columns, next_x = (col + 1) * 412 / columns;
        const bool digit = hex_digit(key.id) >= 0 && (programmer || key.id <= '9');
        const uint32_t fill = key.id == '=' ? 0x64D2C5 : digit ? 0x1D2B45 : 0x294565;
        const lv_font_t *font = (programmer && strlen(key.text) >= 3 && key.id != CALC_BACKSPACE) ?
            math_font(false) : math_font(true);
        lv_obj_t *button = make_key(ui, calc->keypad, key, x, row * step, next_x - x - 4, step - 3, fill, font);
        if (programmer && digit) calc->digit_keys[hex_digit(key.id)] = button;
        if (alternate) calc->functions[i - 2] = button;
        if (key.id == CALC_ANGLE) calc->angle_label = lv_obj_get_child(button, 0);
        if (key.id == CALC_SECOND) {
            calc->second_key = button;
            lv_obj_set_state(button, LV_STATE_CHECKED, calc->second);
        }
    }
    if (lv_obj_get_group(calc->readout)) lv_group_focus_obj(calc->readout);
}

static void calculator_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

void app_open_calculator(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "计算器");
    calculator_t *calc = calloc(1, sizeof(*calc));
    if (!calc) {
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF9C85, app_ui_chinese_font());
        return;
    }
    clear_calculation(calc);
    calc->mode = CALC_STANDARD;
    calc->degrees = true;
    calc->programmer = (programmer_t){.radix = 10, .bits = 32};
    ui->page_context = calc;
    ui->page_cleanup = calculator_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(content, 10, 0);

    lv_obj_t *readout = lv_obj_create(content);
    calc->readout = readout;
    lv_obj_set_pos(readout, 0, 39);
    lv_obj_set_size(readout, 408, 76);
    lv_obj_remove_flag(readout, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_pad_all(readout, 0, 0);
    lv_obj_set_style_border_width(readout, 1, 0);
    lv_obj_set_style_border_color(readout, lv_color_hex(0x304968), 0);
    lv_obj_set_style_radius(readout, 16, 0);
    lv_obj_set_style_bg_color(readout, lv_color_hex(0x162641), 0);
    calc->formula = app_ui_add_text(readout, "", LV_ALIGN_TOP_RIGHT, -12, 3, 0xAFC1DD, math_font(false));
    calc->result = app_ui_add_text(readout, "0", LV_ALIGN_BOTTOM_RIGHT, -13, -1, 0xF2F7FF, &lv_font_montserrat_48);
    lv_obj_set_width(calc->formula, 382);
    lv_obj_set_width(calc->result, 382);
    lv_obj_set_style_text_align(calc->formula, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(calc->result, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(calc->formula, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(calc->result, LV_LABEL_LONG_CLIP);
    lv_obj_add_event_cb(readout, calc_keyboard, LV_EVENT_KEY, ui);

    static const calc_key_t modes[] = {
        {CALC_STANDARD, "标准"}, {CALC_SCIENTIFIC, "科学"}, {CALC_PROGRAMMER, "进制"},
    };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        calc->tabs[i] = make_key(ui, content, modes[i], i * 138, 0, 132, 34, 0x223653, math_font(true));

    calc->radix_row = lv_obj_create(content);
    lv_obj_remove_style_all(calc->radix_row);
    lv_obj_remove_flag(calc->radix_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(calc->radix_row, 0, 117);
    lv_obj_set_size(calc->radix_row, 408, 36);
    for (unsigned i = 0; i < 4; ++i) {
        const calc_key_t radix = {CALC_RADIX_BIN + i, radix_labels[i]};
        calc->radices[i] = make_key(ui, calc->radix_row, radix, i * 80, 0, 76, 36, 0x223653, math_font(false));
    }
    lv_obj_t *word = make_key(ui, calc->radix_row, (calc_key_t){CALC_WORD, "32 位"}, 320, 0, 88, 36,
                              0x294565, math_font(false));
    calc->word_label = lv_obj_get_child(word, 0);

    calc->keypad = lv_obj_create(content);
    lv_obj_remove_style_all(calc->keypad);
    lv_obj_remove_flag(calc->keypad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(calc->keypad, 408, 252);

    lv_group_t *group = lv_group_get_default();
    if (group) lv_group_add_obj(group, readout);
    draw_keypad(ui);
    refresh_calculator(calc);
}
