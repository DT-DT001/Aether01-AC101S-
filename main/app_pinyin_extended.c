#include "app_pinyin_extended.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#endif

enum {
    DICTIONARY_ADDRESS = 0x900000,
    DICTIONARY_LIMIT = 0x300000,
    HEADER_SIZE = 40,
    TABLE_SLOT_SIZE = 8,
    SPELLING_LIMIT = 31,
};

typedef struct {
    const uint8_t *table, *keys, *words;
    uint32_t key_count, word_count;
} catalog_t;

static catalog_t catalog;
static atomic_bool available;

static uint32_t read_u32(const uint8_t *blob)
{
    return blob[0] | ((uint32_t)blob[1] << 8) | ((uint32_t)blob[2] << 16) | ((uint32_t)blob[3] << 24);
}

static uint32_t catalog_size(const uint8_t *header, size_t limit)
{
    if (memcmp(header, "PYN2", 4) || read_u32(header + 4) != 1 || read_u32(header + 36)) return 0;
    const uint32_t size = read_u32(header + 8);
    return size > HEADER_SIZE && size <= DICTIONARY_LIMIT && size <= limit ? size : 0;
}

static uint32_t catalog_crc(const uint8_t *bytes, uint32_t size)
{
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(0, bytes, size);
#else
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1U ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
#endif
}

static bool valid_key(const uint8_t *key, size_t size)
{
    if (size < 3 || size > SPELLING_LIMIT + 1 || key[size - 1]) return false;
    for (size_t i = 0; i + 1 < size; ++i)
        if (key[i] < 'a' || key[i] > 'z') return false;
    return true;
}

static bool valid_word_range(const uint8_t *word, const uint8_t *end, uint32_t *records)
{
    for (; word < end; ++*records) {
        const size_t size = 1 + 2 * ((word[0] >> 6) + 2U);
        if ((size_t)(end - word) < size) return false;
        for (size_t i = 1; i < size; i += 2) {
            const unsigned cp = word[i] | ((unsigned)word[i + 1] << 8);
            if ((cp < 0x3400 || cp > 0x9fff) && (cp < 0xf900 || cp > 0xfaff)) return false;
        }
        word += size;
    }
    return true;
}

static bool validate_rows(const catalog_t *layout, uint32_t key_bytes, uint32_t word_bytes)
{
    uint32_t k = 0, w = 0, records = 0;
    const uint8_t *slot = layout->table;
    if (read_u32(slot) || read_u32(slot + 4)) return false;
    const char *previous = NULL;
    for (uint32_t index = 0; index < layout->key_count; ++index) {
        slot += TABLE_SLOT_SIZE;
        const uint32_t next_k = read_u32(slot), next_w = read_u32(slot + 4);
        if (next_k <= k || next_k > key_bytes || next_w <= w || next_w > word_bytes) return false;
        const uint8_t *key = layout->keys + k;
        if (!valid_key(key, next_k - k) || (previous && strcmp(previous, (const char *)key) >= 0) ||
            !valid_word_range(layout->words + w, layout->words + next_w, &records)) return false;
        previous = (const char *)key;
        k = next_k;
        w = next_w;
    }
    return k == key_bytes && w == word_bytes && records == layout->word_count;
}

static bool open_catalog(const uint8_t *data, size_t length)
{
    if (!data || length < HEADER_SIZE) return false;
    const uint32_t size = catalog_size(data, length);
    if (!size) return false;
    const uint32_t key_count = read_u32(data + 16), word_count = read_u32(data + 20);
    const uint32_t keys_offset = read_u32(data + 28), words_offset = read_u32(data + 32);
    if (!key_count || key_count > (size - HEADER_SIZE) / TABLE_SLOT_SIZE || word_count < key_count ||
        read_u32(data + 24) != HEADER_SIZE || keys_offset != HEADER_SIZE + (key_count + 1) * TABLE_SLOT_SIZE ||
        keys_offset >= words_offset || words_offset >= size ||
        read_u32(data + 12) != catalog_crc(data + HEADER_SIZE, size - HEADER_SIZE)) return false;

    const catalog_t candidate = {data + HEADER_SIZE, data + keys_offset, data + words_offset, key_count, word_count};
    if (!validate_rows(&candidate, words_offset - keys_offset, size - words_offset)) return false;
    catalog = candidate;
    atomic_store_explicit(&available, true, memory_order_release);
    return true;
}

size_t app_pinyin_extended_count(void)
{
    return atomic_load_explicit(&available, memory_order_acquire) ? catalog.key_count : 0;
}

const char *app_pinyin_extended_key(size_t index)
{
    if (index >= app_pinyin_extended_count()) return NULL;
    return (const char *)(catalog.keys + read_u32(catalog.table + index * TABLE_SLOT_SIZE));
}

const uint8_t *app_pinyin_extended_words(size_t index, const uint8_t **end)
{
    if (end) *end = NULL;
    if (index >= app_pinyin_extended_count()) return NULL;
    const uint8_t *slot = catalog.table + index * TABLE_SLOT_SIZE + 4;
    if (end) *end = catalog.words + read_u32(slot + TABLE_SLOT_SIZE);
    return catalog.words + read_u32(slot);
}

#ifdef ESP_PLATFORM
bool app_pinyin_extended_init(void)
{
    if (app_pinyin_extended_count()) return true;
    const esp_partition_t *storage = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!storage || storage->address > DICTIONARY_ADDRESS) return false;
    const size_t offset = DICTIONARY_ADDRESS - storage->address;
    if (offset > storage->size || storage->size - offset < DICTIONARY_LIMIT) return false;

    uint8_t header[HEADER_SIZE];
    if (esp_partition_read(storage, offset, header, sizeof(header)) != ESP_OK) return false;
    const uint32_t size = catalog_size(header, DICTIONARY_LIMIT);
    if (!size) return false;

    const void *data;
    esp_partition_mmap_handle_t mapping;
    if (esp_partition_mmap(storage, offset, size, ESP_PARTITION_MMAP_DATA, &data, &mapping) != ESP_OK) {
        ESP_LOGW("pinyin_ext", "Cannot map extension, using built-in dictionary");
        return false;
    }
    if (!open_catalog(data, size)) {
        esp_partition_munmap(mapping);
        ESP_LOGW("pinyin_ext", "Invalid extension, using built-in dictionary");
        return false;
    }
    ESP_LOGI("pinyin_ext", "Loaded extended Pinyin dictionary: keys=%lu entries=%lu bytes=%lu",
             (unsigned long)catalog.key_count, (unsigned long)catalog.word_count, (unsigned long)size);
    return true;
}
#endif

#ifdef APP_PINYIN_EXTENDED_TEST
bool app_pinyin_extended_test_load(const uint8_t *data, size_t size)
{
    atomic_store_explicit(&available, false, memory_order_release);
    return open_catalog(data, size);
}
#endif
