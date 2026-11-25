#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_CALC_PI 3.14159265358979323846
#define APP_CALC_E  2.71828182845904523536

typedef enum {
    APP_CALC_SIN, APP_CALC_COS, APP_CALC_TAN,
    APP_CALC_ASIN, APP_CALC_ACOS, APP_CALC_ATAN,
    APP_CALC_LOG10, APP_CALC_LN, APP_CALC_SQRT, APP_CALC_CBRT,
    APP_CALC_SQUARE, APP_CALC_CUBE, APP_CALC_RECIPROCAL,
    APP_CALC_EXP, APP_CALC_POW10, APP_CALC_ABS, APP_CALC_FACTORIAL,
} app_calc_scientific_op_t;

/* False leaves the output unchanged for invalid operands or a nonfinite result. */
bool app_calc_scientific(app_calc_scientific_op_t op, double input, bool degrees, double *output);
bool app_calc_power(double base, double exponent, double *output);

typedef enum {
    APP_CALC_ADD, APP_CALC_SUBTRACT, APP_CALC_MULTIPLY, APP_CALC_DIVIDE, APP_CALC_MOD,
    APP_CALC_AND, APP_CALC_OR, APP_CALC_XOR, APP_CALC_NOT, APP_CALC_SHL, APP_CALC_SHR,
} app_calc_program_op_t;

enum { APP_CALC_PROGRAM_TEXT_SIZE = 65 };

/* Words use 8, 16, 32 or 64 unsigned bits. Arithmetic wraps to the word size;
 * shifts are logical, with a count at or above the word size returning zero. */
uint64_t app_calc_programmer_mask(unsigned bits);
bool app_calc_programmer_apply(app_calc_program_op_t op, uint64_t lhs, uint64_t rhs,
                               unsigned bits, uint64_t *output);
/* Parse bare digits in base 2, 8, 10 or 16, rejecting values outside the word. */
bool app_calc_programmer_parse(const char *text, unsigned radix, unsigned bits, uint64_t *output);
bool app_calc_programmer_format(uint64_t value, unsigned radix, unsigned bits,
                                 char output[APP_CALC_PROGRAM_TEXT_SIZE]);
