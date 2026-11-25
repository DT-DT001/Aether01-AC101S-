#include "app_calc_modes.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void scientific(app_calc_scientific_op_t op, double value, bool degrees, double expected)
{
    double out = NAN;
    assert(app_calc_scientific(op, value, degrees, &out));
    assert(fabs(out - expected) <= 2e-14 * fmax(1, fabs(expected)));
}

static void test_scientific(void)
{
    scientific(APP_CALC_SIN, 30, true, 0.5);
    scientific(APP_CALC_SIN, -270, true, 1);
    scientific(APP_CALC_SIN, 360000030, true, 0.5);
    scientific(APP_CALC_SIN, 1e100, false, sin(1e100));
    scientific(APP_CALC_COS, 90, true, 0);
    scientific(APP_CALC_COS, APP_CALC_PI, false, -1);
    scientific(APP_CALC_TAN, 45, true, 1);
    scientific(APP_CALC_TAN, APP_CALC_PI / 4, false, 1);
    scientific(APP_CALC_ASIN, 1, true, 90);
    scientific(APP_CALC_ACOS, -1, false, APP_CALC_PI);
    scientific(APP_CALC_ATAN, -1, true, -45);
    scientific(APP_CALC_LOG10, 1000, false, 3);
    scientific(APP_CALC_LN, APP_CALC_E, false, 1);
    scientific(APP_CALC_SQRT, 81, false, 9);
    scientific(APP_CALC_CBRT, -27, false, -3);
    scientific(APP_CALC_SQUARE, -7, false, 49);
    scientific(APP_CALC_CUBE, -4, false, -64);
    scientific(APP_CALC_RECIPROCAL, 8, false, 0.125);
    scientific(APP_CALC_ABS, -8, false, 8);
    scientific(APP_CALC_EXP, 1, false, APP_CALC_E);
    scientific(APP_CALC_POW10, -3, false, 0.001);
    scientific(APP_CALC_FACTORIAL, 0, false, 1);
    scientific(APP_CALC_FACTORIAL, 10, false, 3628800);
    scientific(APP_CALC_FACTORIAL, 170, false, 7.257415615307999e306);

    static const struct { app_calc_scientific_op_t op; double value; bool degrees; } invalid[] = {
        {APP_CALC_TAN, 90, true}, {APP_CALC_TAN, -270, true},
        {APP_CALC_TAN, 3 * APP_CALC_PI / 2, false}, {APP_CALC_ASIN, 1.01, true},
        {APP_CALC_ACOS, -1.01, false}, {APP_CALC_LN, 0, false}, {APP_CALC_LOG10, -10, false},
        {APP_CALC_SQRT, -1, false}, {APP_CALC_RECIPROCAL, -0.0, false},
        {APP_CALC_EXP, 1000, false}, {APP_CALC_SQUARE, DBL_MAX, false},
        {APP_CALC_FACTORIAL, 171, false}, {APP_CALC_FACTORIAL, 3.5, false},
        {APP_CALC_FACTORIAL, -1, false}, {APP_CALC_ABS, NAN, false}, {APP_CALC_ABS, INFINITY, false},
        {(app_calc_scientific_op_t)99, 1, false},
    };
    double out = 123;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!app_calc_scientific(invalid[i].op, invalid[i].value, invalid[i].degrees, &out) && out == 123);
    }
    assert(!app_calc_scientific(APP_CALC_CBRT, 8, false, NULL));
    assert(!app_calc_power(-2, 0.5, &out) && out == 123);
    assert(!app_calc_power(0, -1, &out) && out == 123);
    assert(!app_calc_power(10, 400, &out) && out == 123);
    assert(!app_calc_power(2, INFINITY, &out) && out == 123);
    assert(app_calc_power(-3, 3, &out) && out == -27);
    assert(app_calc_power(9, 0.5, &out) && out == 3);
    assert(app_calc_power(0, 0, &out) && out == 1);
    assert(!app_calc_power(1, 1, NULL));
}

static uint64_t apply(app_calc_program_op_t op, uint64_t lhs, uint64_t rhs, unsigned bits)
{
    uint64_t out = UINT64_MAX;
    assert(app_calc_programmer_apply(op, lhs, rhs, bits, &out));
    return out;
}

static void test_word_arithmetic(void)
{
    const uint64_t masks[] = {UINT8_MAX, UINT16_MAX, UINT32_MAX, UINT64_MAX};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned bits = 8U << i;
        const uint64_t all = masks[i];
        assert(app_calc_programmer_mask(bits) == all);
        assert(apply(APP_CALC_ADD, all, 1, bits) == 0);
        assert(apply(APP_CALC_SUBTRACT, 0, 1, bits) == all);
        assert(apply(APP_CALC_MULTIPLY, all, 2, bits) == all - 1);
        assert(apply(APP_CALC_DIVIDE, all, 3, bits) == all / 3);
        assert(apply(APP_CALC_MOD, all, 13, bits) == all % 13);
        assert(apply(APP_CALC_AND, 0xCA, 0xA7, bits) == 0x82);
        assert(apply(APP_CALC_OR, 0xCA, 0xA7, bits) == 0xEF);
        assert(apply(APP_CALC_XOR, 0xCA, 0xA7, bits) == 0x6D);
        assert(apply(APP_CALC_NOT, 0x27, 0, bits) == (all ^ 0x27));
        const uint64_t top = (uint64_t)1 << (bits - 1);
        assert(apply(APP_CALC_SHL, 1, bits - 1, bits) == top);
        assert(apply(APP_CALC_SHR, top, bits - 1, bits) == 1);
        assert(apply(APP_CALC_SHL, 1, bits, bits) == 0);
        assert(apply(APP_CALC_SHR, all, UINT64_MAX, bits) == 0);
        assert(apply(APP_CALC_SHL, top, 1, bits) == 0);
        assert(apply(APP_CALC_SHR, all, 0, bits) == all);
        assert(apply(APP_CALC_SHL, 33, 256, bits) == 0);
    }
    for (unsigned a = 0; a <= 255; ++a) {
        for (unsigned b = 0; b <= 255; ++b) {
            assert(apply(APP_CALC_ADD, a, b, 8) == (uint8_t)(a + b));
            assert(apply(APP_CALC_SUBTRACT, a, b, 8) == (uint8_t)(a - b));
            assert(apply(APP_CALC_MULTIPLY, a, b, 8) == (uint8_t)(a * b));
            if (b) {
                assert(apply(APP_CALC_DIVIDE, a, b, 8) == a / b);
                assert(apply(APP_CALC_MOD, a, b, 8) == a % b);
            }
        }
    }
    uint64_t out = 123;
    assert(!app_calc_programmer_apply(APP_CALC_DIVIDE, 8, 0, 32, &out));
    assert(!app_calc_programmer_apply(APP_CALC_MOD, 8, 256, 8, &out));
    assert(!app_calc_programmer_apply(APP_CALC_DIVIDE, 3, 3, 7, &out));
    assert(!app_calc_programmer_apply(APP_CALC_NOT, 8, 0, 64, NULL));
    assert(!app_calc_programmer_apply((app_calc_program_op_t)99, 8, 4, 64, &out));
    assert(out == 123);
}

static void test_conversion(void)
{
    char text[APP_CALC_PROGRAM_TEXT_SIZE];
    uint64_t result;
    assert(app_calc_programmer_parse("00fF", 16, 8, &result) && result == 255);
    assert(app_calc_programmer_format(256, 10, 8, text) && !strcmp(text, "0"));
    assert(app_calc_programmer_format(UINT64_MAX, 16, 64, text) && !strcmp(text, "FFFFFFFFFFFFFFFF"));
    assert(app_calc_programmer_format(UINT64_MAX, 2, 64, text) && strlen(text) == 64 && strspn(text, "1") == 64);
    assert(app_calc_programmer_format(UINT64_MAX, 10, 64, text) && !strcmp(text, "18446744073709551615"));
    assert(app_calc_programmer_format(UINT64_MAX, 8, 64, text) && !strcmp(text, "1777777777777777777777"));

    static const struct { const char *digits; unsigned radix, bits; } invalid[] = {
        {"", 2, 8}, {"2", 2, 8}, {"8", 8, 32}, {"A", 10, 64}, {"z3", 16, 8}, {"+1", 16, 32},
        {"-1", 16, 32}, {" 7F", 16, 8}, {"7F ", 16, 8}, {"0xFF", 16, 32}, {"256", 10, 8},
        {"10000", 16, 16}, {"40000000000", 8, 32}, {"18446744073709551616", 10, 64},
        {"2000000000000000000000", 8, 64}, {"10000000000000000", 16, 64}, {"42", 3, 64}, {"42", 16, 0},
    };
    result = 42;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!app_calc_programmer_parse(invalid[i].digits, invalid[i].radix, invalid[i].bits, &result));
        assert(result == 42);
    }
    assert(!app_calc_programmer_parse(NULL, 10, 64, &result) && result == 42);
    assert(!app_calc_programmer_parse("42", 10, 64, NULL));
    strcpy(text, "untouched");
    assert(!app_calc_programmer_format(0, 8, 65, text) && !strcmp(text, "untouched"));
    assert(!app_calc_programmer_format(0, 1, 32, text) && !strcmp(text, "untouched"));
    assert(!app_calc_programmer_format(0, 10, 32, NULL));

    const unsigned radices[] = {2, 8, 10, 16};
    uint64_t seed = UINT64_C(0xc91b73769c255ed9);
    for (unsigned i = 0; i < 10000; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        for (unsigned bits = 8; bits <= 64; bits *= 2) {
            for (unsigned j = 0; j < sizeof(radices) / sizeof(radices[0]); ++j) {
                const unsigned radix = radices[j];
                assert(app_calc_programmer_format(seed, radix, bits, text));
                assert(app_calc_programmer_parse(text, radix, bits, &result));
                assert(result == (seed & app_calc_programmer_mask(bits)));
            }
        }
    }
}

int main(void)
{
    test_scientific();
    test_word_arithmetic();
    test_conversion();
    puts("PASS calculator scientific domains, exhaustive 8-bit arithmetic, 8/16/32/64-bit logic and 160000 radix round trips");
    return 0;
}
