#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool boot_prepare_flash(uint32_t firmware_size);
bool boot_write_chunk(uint32_t offset, const uint8_t* data, uint8_t len, uint32_t firmware_size);
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);
bool boot_metadata_read_transport(uint32_t* packed);
bool boot_metadata_store_transport(uint32_t packed);
void boot_jump_to_application(void);
