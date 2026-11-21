#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "qmi8658a.h"
#include "dps368.h"
#include "freertos/FreeRTOS.h"

struct test_i2c { uint8_t reg[256]; };
static struct test_i2c imu, baro;
static unsigned reads[2][256], writes[2][256], delays;
static TickType_t ticks;
static int failing_reg = -1, dropped_reg = -1, failing_read = -1;
static int selected;
static unsigned imu_burst_crossings;

static void set_imu_counter(uint32_t value)
{
    for (unsigned i = 0; i < 3; ++i) imu.reg[0x30 + i] = (uint8_t)(value >> (8 * i));
}

static unsigned device_index(i2c_master_dev_handle_t d) { assert(d == &imu || d == &baro); return d == &imu ? 0 : 1; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t t) { delays += t; ticks += t; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *d)
{ assert(b && c && d && c->scl_speed_hz == 400000); *d = c->device_address == 0x6B ? &imu : &baro; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { (void)device_index(d); return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *cmd, size_t n, uint8_t *out, size_t bytes, int timeout)
{
    const unsigned i = device_index(d); assert(cmd && n == 1 && out);
    const uint8_t r = cmd[0]; ++reads[i][r];
    assert(timeout == 100 || (i == 0 && timeout == 12 &&
                                ((r == 0x30 && bytes == 3) || (r == 0x35 && bytes == 12))));
    if ((int)i == selected && r == failing_read) { failing_read = -1; return ESP_FAIL; }
    memcpy(out, &d->reg[r], bytes);
    if (i == 0 && r == 0x35 && timeout == 12 && imu_burst_crossings) {
        --imu_burst_crossings;
        ++imu.reg[0x30];
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *cmd, size_t n, int timeout)
{
    const unsigned i = device_index(d); assert(cmd && n == 2 && timeout == 100);
    const uint8_t r = cmd[0]; ++writes[i][r];
    if ((int)i == selected && r == failing_reg) {
        failing_reg = -1; d->reg[r] = cmd[1]; return ESP_FAIL; /* Applied despite lost ACK. */
    }
    if ((int)i == selected && r == dropped_reg) { dropped_reg = -1; return ESP_OK; }
    if (i && r == 8) { assert(!(cmd[1] & 0xF8)); d->reg[r] = (d->reg[r] & 0xF8) | cmd[1]; }
    else d->reg[r] = cmd[1];
    return ESP_OK;
}

static const int16_t level[3] = {0, 0, 8192};
static const int16_t offset[3] = {34, -20, 9};

static void set_axis(unsigned reg, int16_t count)
{
    imu.reg[reg] = (uint16_t)count & 0xffU;
    imu.reg[reg + 1] = (uint16_t)count >> 8;
}

static qmi8658a_sample_t imu_sample(qmi8658a_handle_t handle, TickType_t elapsed,
                                   const int16_t accel[3], const int16_t gyro[3])
{
    ticks += elapsed;
    for (unsigned axis = 0; axis < 3; ++axis) {
        set_axis(0x35 + axis * 2, accel[axis]);
        set_axis(0x3B + axis * 2, gyro[axis]);
    }
    qmi8658a_sample_t sample;
    assert(qmi8658a_read(handle, &sample) == ESP_OK);
    for (unsigned axis = 0; axis < 3; ++axis) {
        assert(sample.gyro_raw_dps[axis] == gyro[axis] / 64.0f);
        assert(sample.gyro_dps[axis] == sample.gyro_raw_dps[axis] - sample.gyro_bias_dps[axis]);
    }
    return sample;
}

static qmi8658a_handle_t fresh_imu(void)
{
    qmi8658a_handle_t handle = NULL;
    selected = 0;
    assert(qmi8658a_new(&imu, &handle) == ESP_OK);
    return handle;
}

static void test_fresh_imu_burst(void)
{
    qmi8658a_handle_t handle = fresh_imu();
    qmi8658a_sample_t sample;
    set_imu_counter(0xFFFFFF);
    set_axis(0x3B, 64);
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_OK);
    assert(sample.gyro_raw_dps[0] == 1.0f);

    set_axis(0x3B, 128);
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_ERR_NOT_FINISHED);
    assert(sample.gyro_raw_dps[0] == 1.0f);
    set_imu_counter(0);
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_OK);
    assert(sample.gyro_raw_dps[0] == 2.0f);

    set_imu_counter(1);
    set_axis(0x3B, 192);
    imu_burst_crossings = 1;
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_OK);
    assert(sample.gyro_raw_dps[0] == 3.0f);
    set_imu_counter(3);
    set_axis(0x3B, 256);
    imu_burst_crossings = 3;
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_ERR_NOT_FINISHED);
    assert(imu_burst_crossings == 1 && sample.gyro_raw_dps[0] == 3.0f);
    imu_burst_crossings = 0;
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_OK && sample.gyro_raw_dps[0] == 4.0f);

    failing_read = 0x30;
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_FAIL && sample.gyro_raw_dps[0] == 4.0f);
    assert(qmi8658a_set_sleep(handle, true) == ESP_OK);
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_ERR_INVALID_STATE);
    assert(qmi8658a_set_sleep(handle, false) == ESP_OK);
    assert(qmi8658a_read_fresh(handle, &sample) == ESP_OK);
    assert(qmi8658a_del(handle) == ESP_OK);
    puts("PASS QMI fast reads reject duplicates and crossed bursts, retry at most once, wrap and reset across sleep");
}

static void test_large_gyro_bias(void)
{
    const int16_t bench_accel[3] = {301, 541, 7934};
    const int16_t bench_gyro[3] = {116, 420, 15};
    qmi8658a_handle_t handle = fresh_imu();
    qmi8658a_sample_t sample;
    for (unsigned i = 0; i <= 60; ++i) {
        sample = imu_sample(handle, 20, bench_accel, bench_gyro);
        assert(sample.gyro_bias_valid == (i == 60));
        assert(sample.gyro_stationary == (i == 60));
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        assert(sample.gyro_bias_dps[axis] == bench_gyro[axis] / 64.0f);
        assert(sample.gyro_dps[axis] == 0.0f);
    }

    /* The same learned sensor-frame bias can be parallel to gravity after a
     * reorientation. Requalification uses the residual, not the cold gate. */
    const int16_t side[3] = {0, 8192, 0};
    for (unsigned i = 0; i <= 60; ++i) {
        sample = imu_sample(handle, 20, side, bench_gyro);
        assert(sample.gyro_bias_valid);
        assert(sample.gyro_stationary == (i == 60));
    }
    assert(qmi8658a_del(handle) == ESP_OK);

    handle = fresh_imu();
    const int16_t transverse_gyro[3] = {0, 420, 0};
    for (unsigned i = 0; i < 150; ++i) {
        /* Gravity in sensor coordinates during a 6.5625 dps turn about Y. */
        const float angle = i * 0.02f * (420.0f / 64.0f) * (3.14159265f / 180.0f);
        const int16_t rotated[3] = {
            (int16_t)lroundf(-8192.0f * sinf(angle)), 0, (int16_t)lroundf(8192.0f * cosf(angle)),
        };
        sample = imu_sample(handle, 20, rotated, transverse_gyro);
        assert(!sample.gyro_bias_valid && !sample.gyro_stationary);
    }
    const int16_t yaw[3] = {0, 0, -420};
    const int16_t over_limit[3] = {0, 800, 0};
    for (unsigned i = 0; i < 90; ++i) {
        sample = imu_sample(handle, 20, level, yaw);
        assert(!sample.gyro_bias_valid && !sample.gyro_stationary);
    }
    for (unsigned i = 0; i < 90; ++i) {
        sample = imu_sample(handle, 20, level, over_limit);
        assert(!sample.gyro_bias_valid && !sample.gyro_stationary);
    }
    assert(qmi8658a_del(handle) == ESP_OK);
    puts("PASS QMI measured cold bias, reorientation, sustained tilt rotation, large yaw and raw bias ceiling");
}

static void test_gyro_calibration(void)
{
    qmi8658a_handle_t handle = fresh_imu();
    qmi8658a_sample_t sample;
    ticks = UINT32_MAX - 500;
    for (unsigned i = 0; i <= 60; ++i) {
        const int16_t noisy[] = {offset[0] + (int)(i % 5) - 2, offset[1], offset[2]};
        sample = imu_sample(handle, 20, level, noisy);
        assert(sample.gyro_bias_valid == (i == 60));
        assert(sample.gyro_stationary == (i == 60));
        if (i < 60) assert(sample.gyro_dps[0] == sample.gyro_raw_dps[0]);
    }
    assert(fabsf(sample.accel_mps2[2] - 9.80665f) < .0001f);
    for (unsigned axis = 0; axis < 3; ++axis) {
        assert(fabsf(sample.gyro_bias_dps[axis] - offset[axis] / 64.0f) < .001f);
        assert(fabsf(sample.gyro_dps[axis]) < .035f);
    }
    puts("PASS QMI gyro stationary qualification across tick wrap, all three axes and raw/bias/corrected sample diagnostics");

    float learned[3];
    memcpy(learned, sample.gyro_bias_dps, sizeof(learned));
    const int16_t turning[3] = {offset[0], offset[1], offset[2] + 65};
    const int16_t tilted[3] = {8192, 0, 0};
    for (unsigned i = 0; i < 150; ++i) {
        sample = imu_sample(handle, 20, level, turning);
        assert(sample.gyro_bias_valid && !sample.gyro_stationary);
        assert(!memcmp(learned, sample.gyro_bias_dps, sizeof(learned)));
    }
    for (unsigned i = 0; i < 150; ++i) {
        sample = imu_sample(handle, 20, (i & 1U) ? level : tilted, offset);
        assert(!sample.gyro_stationary && !memcmp(learned, sample.gyro_bias_dps, sizeof(learned)));
    }
    puts("PASS QMI learned bias freezes on residual rotation and gravity-direction motion at constant acceleration magnitude");

    const int16_t drift[3] = {offset[0] + 12, offset[1] - 6, offset[2] + 4};
    for (unsigned i = 0; i <= 60; ++i) {
        sample = imu_sample(handle, i == 0 ? 101 : 20, level, drift);
        assert(!memcmp(learned, sample.gyro_bias_dps, sizeof(learned)));
    }
    assert(sample.gyro_stationary);
    sample = imu_sample(handle, 20, level, drift);
    assert(sample.gyro_bias_dps[0] > learned[0] && sample.gyro_bias_dps[0] - learned[0] < .002f);
    for (unsigned i = 0; i < 1500; ++i) sample = imu_sample(handle, 20, level, drift);
    for (unsigned axis = 0; axis < 3; ++axis) assert(fabsf(sample.gyro_dps[axis]) < .002f);
    puts("PASS QMI requalification preserves prior estimate, then tracks drift with bounded time-scaled correction");

    memcpy(learned, sample.gyro_bias_dps, sizeof(learned));
    sample = imu_sample(handle, 101, level, drift);
    assert(sample.gyro_bias_valid && !sample.gyro_stationary);
    for (unsigned i = 0; i < 59; ++i) sample = imu_sample(handle, 20, level, drift);
    failing_read = 0x35;
    assert(qmi8658a_read(handle, &sample) == ESP_FAIL);
    sample = imu_sample(handle, 20, level, drift);
    assert(sample.gyro_bias_valid && !sample.gyro_stationary);
    for (unsigned i = 0; i < 60; ++i) sample = imu_sample(handle, 20, level, drift);
    assert(sample.gyro_stationary);
    assert(qmi8658a_set_sleep(handle, true) == ESP_OK);
    assert(qmi8658a_read(handle, &sample) == ESP_ERR_INVALID_STATE);
    assert(qmi8658a_set_sleep(handle, false) == ESP_OK);
    sample = imu_sample(handle, 20, level, drift);
    assert(sample.gyro_bias_valid && !sample.gyro_stationary);
    assert(!memcmp(learned, sample.gyro_bias_dps, sizeof(learned)));
    assert(qmi8658a_del(handle) == ESP_OK);
    puts("PASS QMI gap, read fault and sleep reset stationary evidence without erasing the learned bias");

    handle = fresh_imu();
    const int16_t fast_turn[3] = {0, 0, 192};
    const int16_t shaking[3] = {0, 0, 9000};
    for (unsigned i = 0; i < 90; ++i) {
        sample = imu_sample(handle, 20, level, fast_turn);
        assert(!sample.gyro_bias_valid);
        sample = imu_sample(handle, 20, shaking, offset);
        assert(!sample.gyro_bias_valid);
    }
    sample = imu_sample(handle, 20, level, offset);
    for (unsigned i = 0; i < 100; ++i) sample = imu_sample(handle, 0, level, offset);
    for (unsigned i = 0; i < 38; ++i) sample = imu_sample(handle, 50, level, offset);
    assert(!sample.gyro_bias_valid); /* >1.2s, but only 39 distinct samples. */
    sample = imu_sample(handle, 50, level, offset);
    assert(sample.gyro_bias_valid && sample.gyro_stationary);
    assert(qmi8658a_del(handle) == ESP_OK);
    puts("PASS QMI movement never initializes bias and duplicate timestamps cannot qualify a sparse stationary window");
}
int main(void)
{
    imu.reg[0] = 5; imu.reg[2] = 0x41; /* Retained power-down from earlier boot. */
    baro.reg[8] = 0xC0;
    qmi8658a_handle_t q = NULL; dps368_handle_t d = NULL;
    assert(qmi8658a_new(&imu, &q) == ESP_OK && imu.reg[2] == 0x40 && imu.reg[8] == 3 && delays >= 40);
    assert(dps368_new(&baro, &d) == ESP_OK && (baro.reg[8] & 7) == 7);
    imu.reg[2] = 0x58; imu.reg[8] = 0xA3;
    assert(qmi8658a_set_sleep(q, true) == ESP_OK && imu.reg[2] == 0x41 && imu.reg[8] == 0xA0);
    qmi8658a_sample_t qs; assert(qmi8658a_read(q, &qs) == ESP_ERR_INVALID_STATE);
    assert(qmi8658a_set_sleep(q, true) == ESP_OK); /* Does not replace awake snapshot. */
    assert(qmi8658a_set_sleep(q, false) == ESP_OK && imu.reg[2] == 0x58 && imu.reg[8] == 0xA3);
    assert(qmi8658a_read(q, &qs) == ESP_OK);
    puts("PASS real QMI driver retained-boot wake, sensor/oscillator power-down readback and exact idempotent restore");

    for (unsigned k = 0; k < 6; ++k) {
        imu.reg[2] = 0x58; imu.reg[8] = 0xA3; selected = 0;
        if (k == 0) failing_read = 2;
        if (k == 1) failing_read = 8;
        if (k == 2) dropped_reg = 8;
        if (k == 3) failing_reg = 8;
        if (k == 4) dropped_reg = 2;
        if (k == 5) failing_reg = 2;
        assert(qmi8658a_set_sleep(q, true) != ESP_OK);
        assert(qmi8658a_set_sleep(q, false) == ESP_OK && imu.reg[2] == 0x58 && imu.reg[8] == 0xA3);
    }
    selected = 0; assert(qmi8658a_set_sleep(q, true) == ESP_OK); dropped_reg = 2;
    assert(qmi8658a_set_sleep(q, false) == ESP_ERR_INVALID_RESPONSE);
    assert(qmi8658a_read(q, &qs) == ESP_ERR_INVALID_STATE);
    assert(qmi8658a_set_sleep(q, false) == ESP_OK);
    puts("PASS QMI lost ACK, dropped write, read failure and failed restore remain recoverable without stale sampling");

    selected = 1; baro.reg[8] = 0xF7;
    assert(dps368_set_sleep(d, true) == ESP_OK && baro.reg[8] == 0xF0);
    dps368_sample_t ds; assert(dps368_read(d, &ds) == ESP_ERR_INVALID_STATE);
    assert(dps368_set_sleep(d, true) == ESP_OK);
    assert(dps368_set_sleep(d, false) == ESP_OK && baro.reg[8] == 0xF7);
    for (unsigned k = 0; k < 3; ++k) {
        if (k == 0) failing_read = 8;
        if (k == 1) dropped_reg = 8;
        if (k == 2) failing_reg = 8;
        assert(dps368_set_sleep(d, true) != ESP_OK);
        assert(dps368_set_sleep(d, false) == ESP_OK && (baro.reg[8] & 7) == 7);
    }
    assert(dps368_set_sleep(d, true) == ESP_OK); dropped_reg = 8;
    assert(dps368_set_sleep(d, false) == ESP_ERR_INVALID_RESPONSE);
    assert(dps368_read(d, &ds) == ESP_ERR_INVALID_STATE);
    assert(dps368_set_sleep(d, false) == ESP_OK);
    assert(!writes[1][6] || baro.reg[6] == 0x43); /* Sleep never changes calibration/precision. */
    assert(qmi8658a_set_sleep(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(dps368_set_sleep(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(qmi8658a_del(q) == ESP_OK && dps368_del(d) == ESP_OK);
    puts("PASS real DPS standby/restore masks only MEAS_CTRL and retains recoverability after every failure class");
    test_gyro_calibration();
    test_large_gyro_bias();
    test_fresh_imu_burst();
    return 0;
}
