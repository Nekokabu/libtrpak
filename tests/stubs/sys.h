#ifndef TEST_SYS_H
#define TEST_SYS_H

#include <stddef.h>

void dma_write_s(void *source, unsigned long destination, size_t size);
void dma_read_s(void *destination, unsigned long source, size_t size);

#endif
