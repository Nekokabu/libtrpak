#ifndef TEST_LIBDRAGON_H
#define TEST_LIBDRAGON_H

#include <stddef.h>
#include <stdint.h>

int joybus_accessory_read(int controller, uint16_t address, uint8_t *data);
int joybus_accessory_write(
    int controller,
    uint16_t address,
    const uint8_t *data
);
void wait_ms(unsigned int milliseconds);
void data_cache_hit_writeback_invalidate(void *address, size_t size);

#endif
