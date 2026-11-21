#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../components/ac101s_audio/ac101s_codec.c"

static uint8_t registers[256];
static int bus, device;
static int reject_read = -1, ignore_write = -1;
static bool attached, no_ack;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
                                   const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *out)
{
    assert(handle == &bus && !attached);
    assert(config->dev_addr_length == 7 && config->device_address == 0x33);
    assert(config->scl_speed_hz == 100000);
    *out = &device;
    attached = true;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    assert(dev == &device && attached);
    attached = false;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, int timeout)
{
    assert(dev == &device && attached && size == 2 && timeout == 10);
    if (no_ack) return ESP_ERR_TIMEOUT;
    if (!data[0]) {
        assert(data[1] == 0x34);
        memset(registers, 0, sizeof(registers));
    } else if (data[0] != ignore_write) {
        registers[data[0]] = data[1];
    }
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *reg, size_t tx_size,
                                     uint8_t *data, size_t rx_size, int timeout)
{
    assert(dev == &device && attached && tx_size == 1 && rx_size == 1 && timeout == 10);
    if (*reg == reject_read) return ESP_ERR_TIMEOUT;
    *data = registers[*reg];
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks) { assert(ticks >= 20); }

int main(void)
{
    i2c_master_dev_handle_t dev = NULL;
    no_ack = true;
    assert(ac101s_codec_new(&bus, &dev) == ESP_ERR_TIMEOUT && !attached && !dev);
    no_ack = false;
    ignore_write = 0x0c;
    assert(ac101s_codec_new(&bus, &dev) == ESP_ERR_INVALID_RESPONSE && !attached && !dev);
    ignore_write = -1;
    assert(ac101s_codec_new(&bus, &dev) == ESP_OK && attached && dev);
    assert(registers[1] == 0x73 && registers[2] == 0x1b && !registers[6]);
    assert(registers[4] == 1 && registers[5] == 1 && registers[0x07] == 0);
    assert(registers[0x0a] == 15 && registers[0x0b] == 0x14 && registers[0x0c] == 0x33);
    assert(registers[0x0e] == 5 && registers[0x0f] == 3 && registers[0x18] == 0xf);
    for (unsigned i = 0; i < 100; ++i) {
        assert(ac101s_codec_start(dev, false, 42) == ESP_OK);
        assert(registers[6] == 0x35 && registers[7] == 0x17 && registers[0x20] == 0xf9);
        assert((registers[0x25] & 0x64) == 0x64 && registers[0x26] == 0x20 + (i ? 5 : 0));
        assert(ac101s_codec_set_speaker(dev, 71) == ESP_ERR_INVALID_ARG);
        assert(ac101s_codec_set_speaker(dev, 70) == ESP_OK && registers[0x26] == 0x25);
        assert(!(registers[0x25] & 4));
        assert(ac101s_codec_stop(dev) == ESP_OK);
        assert(!registers[6] && !registers[7] && !(registers[2] & 4));
        assert(ac101s_codec_start(dev, true, 24) == ESP_OK);
        assert(registers[6] == 0x31 && registers[7] == 0x15 && registers[0x20] == 0xc1);
        assert(registers[0x25] == 4 && !(registers[0x26] & 0x30) && !(registers[0x1b] & 1));
        assert(ac101s_codec_stop(dev) == ESP_OK);
    }
    assert(ac101s_codec_start(dev, false, 24) == ESP_OK);
    unsigned previous = 15;
    for (unsigned level = 0; level <= 70; ++level) {
        assert(ac101s_codec_set_speaker(dev, level) == ESP_OK);
        const unsigned gain = registers[0x26] & 0x0f;
        assert(gain >= 5 && gain <= previous && previous - gain <= 1);
        assert((registers[0x26] & 0xf0) == 0x20);
        assert(((registers[0x25] & 4) != 0) == (level == 0));
        if (level == 0) assert(gain == 15);
        if (level == 35) assert(gain == 10);
        if (level == 70) assert(gain == 5);
        previous = gain;
    }
    assert(ac101s_codec_set_speaker(dev, 0) == ESP_OK && (registers[0x25] & 4));
    assert(ac101s_codec_set_speaker(dev, 70) == ESP_OK && !(registers[0x25] & 4));
    assert(ac101s_codec_set_speaker(dev, 0) == ESP_OK && (registers[0x25] & 4));
    ignore_write = 0x26;
    assert(ac101s_codec_set_speaker(dev, 20) == ESP_ERR_INVALID_RESPONSE && (registers[0x25] & 4));
    ignore_write = -1;
    assert(ac101s_codec_set_speaker(dev, 20) == ESP_OK);
    reject_read = 0x25;
    assert(ac101s_codec_stop(dev) == ESP_ERR_TIMEOUT);
    assert(!registers[6] && !registers[7] && !(registers[0x26] & 0x20));
    reject_read = -1;
    assert(ac101s_codec_stop(dev) == ESP_OK && registers[0x25] == 4);
    assert(i2c_master_bus_rm_device(dev) == ESP_OK);
    puts("PASS AC101S address, I2S settings, 100 playback/capture pairs, mute/gain readback and stop retry");
    return 0;
}
