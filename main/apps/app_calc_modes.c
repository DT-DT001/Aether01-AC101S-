#include "app_calc_modes.h"

#include <math.h>

static bool store_number(double value, double *output)
{
    if (!isfinite(value)) return false;
    *output = value == 0 ? 0 : value;
    return true;
}

static double trig(app_calc_scientific_op_t op, double input, bool degrees)
{
    const double half_turn = degrees ? 180.0 : APP_CALC_PI;
    const double reduced = remainder(input, 2 * half_turn);
    /* Keep exact quadrants while the radian math library still sees the full
     * input, so trigonometry on large radians retains accurate range reduction. */
    if (degrees || fabs(input) <= 4 * APP_CALC_PI) {
        if (reduced == 0 || fabs(reduced) == half_turn)
            return op == APP_CALC_COS ? (reduced == 0 ? 1 : -1) : 0;
        if (fabs(reduced) == half_turn / 2) {
            if (op == APP_CALC_TAN) return NAN;
            return op == APP_CALC_COS ? 0 : reduced < 0 ? -1 : 1;
        }
    }
    const double angle = degrees ? reduced * (APP_CALC_PI / 180.0) : input;
    if (op == APP_CALC_SIN) return sin(angle);
    if (op == APP_CALC_COS) return cos(angle);
    return tan(angle);
}

bool app_calc_scientific(app_calc_scientific_op_t op, double input, bool degrees, double *output)
{
    if (!output || !isfinite(input)) return false;
    const double angle_factor = degrees ? 180.0 / APP_CALC_PI : 1.0;
    double result;
    switch (op) {
    case APP_CALC_SIN:
    case APP_CALC_COS:
    case APP_CALC_TAN: result = trig(op, input, degrees); break;
    case APP_CALC_ASIN: result = asin(input) * angle_factor; break;
    case APP_CALC_ACOS: result = acos(input) * angle_factor; break;
    case APP_CALC_ATAN: result = atan(input) * angle_factor; break;
    case APP_CALC_LOG10: result = log10(input); break;
    case APP_CALC_LN: result = log(input); break;
    case APP_CALC_SQRT: result = sqrt(input); break;
    case APP_CALC_CBRT: result = cbrt(input); break;
    case APP_CALC_SQUARE: result = input * input; break;
    case APP_CALC_CUBE: result = input * input * input; break;
    case APP_CALC_RECIPROCAL:
        if (input == 0) return false;
        result = 1 / input;
        break;
    case APP_CALC_EXP: result = exp(input); break;
    case APP_CALC_POW10: result = pow(10.0, input); break;
    case APP_CALC_ABS: result = fabs(input); break;
    case APP_CALC_FACTORIAL:
        if (input < 0 || input > 170 || input != floor(input)) return false;
        result = 1;
        for (unsigned i = 2; i <= (unsigned)input; ++i) result *= i;
        break;
    default: return false;
    }
    return store_number(result, output);
}

bool app_calc_power(double base, double exponent, double *output)
{
    return output && isfinite(base) && isfinite(exponent) && store_number(pow(base, exponent), output);
}

uint64_t app_calc_programmer_mask(unsigned bits)
{
    switch (bits) {
    case 8: return UINT8_MAX;
    case 16: return UINT16_MAX;
    case 32: return UINT32_MAX;
    case 64: return UINT64_MAX;
    default: return 0;
    }
}

bool app_calc_programmer_apply(app_calc_program_op_t op, uint64_t lhs, uint64_t rhs,
                               unsigned bits, uint64_t *output)
{
    const uint64_t mask = app_calc_programmer_mask(bits);
    if (!output || !mask) return false;
    lhs &= mask;
    const uint64_t right = rhs & mask;
    uint64_t result;
    switch (op) {
    case APP_CALC_ADD: result = lhs + right; break;
    case APP_CALC_SUBTRACT: result = lhs - right; break;
    case APP_CALC_MULTIPLY: result = lhs * right; break;
    case APP_CALC_DIVIDE:
        if (!right) return false;
        result = lhs / right;
        break;
    case APP_CALC_MOD:
        if (!right) return false;
        result = lhs % right;
        break;
    case APP_CALC_AND: result = lhs & right; break;
    case APP_CALC_OR: result = lhs | right; break;
    case APP_CALC_XOR: result = lhs ^ right; break;
    case APP_CALC_NOT: result = ~lhs; break;
    case APP_CALC_SHL: result = rhs < bits ? lhs << rhs : 0; break;
    case APP_CALC_SHR: result = rhs < bits ? lhs >> rhs : 0; break;
    default: return false;
    }
    *output = result & mask;
    return true;
}

static bool valid_radix(unsigned radix)
{
    return radix == 2 || radix == 8 || radix == 10 || radix == 16;
}

static unsigned digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return 16;
}

bool app_calc_programmer_parse(const char *text, unsigned radix, unsigned bits, uint64_t *output)
{
    const uint64_t mask = app_calc_programmer_mask(bits);
    if (!output || !text || !*text || !mask || !valid_radix(radix)) return false;
    const uint64_t limit = mask / radix;
    const unsigned last_digit = mask % radix;
    uint64_t value = 0;
    for (; *text; ++text) {
        const unsigned digit = digit_value(*text);
        if (digit >= radix || value > limit || (value == limit && digit > last_digit)) return false;
        value = value * radix + digit;
    }
    *output = value;
    return true;
}

bool app_calc_programmer_format(uint64_t value, unsigned radix, unsigned bits,
                                 char output[APP_CALC_PROGRAM_TEXT_SIZE])
{
    const uint64_t mask = app_calc_programmer_mask(bits);
    if (!output || !mask || !valid_radix(radix)) return false;
    unsigned length = 0;
    value &= mask;
    do {
        const unsigned digit = value % radix;
        output[length++] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value /= radix;
    } while (value);
    output[length] = '\0';
    for (unsigned a = 0, b = length - 1; a < b; ++a, --b) {
        const char ch = output[a];
        output[a] = output[b];
        output[b] = ch;
    }
    return true;
}
