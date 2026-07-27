#include "bootloader_runtime.h"

#include "app.h"

#define CRC32_POLY 0xEDB88320UL
#define FLASH_RAMFUNC_TIMEOUT 8000000UL
#define FLASH_SR_RUNTIME_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                                 FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | \
                                 FLASH_SR_RDERR | FLASH_SR_OPTVERR)

typedef struct {
    uint32_t magic;
    uint32_t packed_transport;
    uint32_t packed_transport_inverse;
    uint32_t magic_inverse;
} BootMetadataRecord;
#define BOOT_RAMFUNC __attribute__((section(".RamFunc"), noinline))

static BOOT_RAMFUNC HAL_StatusTypeDef flash_wait_ready_ram(void) {
    uint32_t timeout = FLASH_RAMFUNC_TIMEOUT;

    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if (timeout-- == 0U) {
            return HAL_TIMEOUT;
        }
    }

    if ((FLASH->SR & FLASH_SR_RUNTIME_ERRORS) != 0U) {
        return HAL_ERROR;
    }

    if ((FLASH->SR & FLASH_SR_EOP) != 0U) {
        FLASH->SR = FLASH_SR_EOP;
    }

    return HAL_OK;
}

static BOOT_RAMFUNC HAL_StatusTypeDef flash_erase_pages_ram(uint32_t first_page, uint32_t page_count, uint32_t* page_error) {
    uint32_t page;

    if (page_error != NULL) {
        *page_error = 0xFFFFFFFFUL;
    }

    for (page = first_page; page < (first_page + page_count); ++page) {
        HAL_StatusTypeDef status;

        status = flash_wait_ready_ram();
        if (status != HAL_OK) {
            if (page_error != NULL) {
                *page_error = page;
            }
            return status;
        }

        FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
        FLASH->CR |= FLASH_CR_PER | ((page << FLASH_CR_PNB_Pos) & FLASH_CR_PNB);
        FLASH->CR |= FLASH_CR_STRT;

        status = flash_wait_ready_ram();
        FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
        if (status != HAL_OK) {
            if (page_error != NULL) {
                *page_error = page;
            }
            return status;
        }
    }

    return HAL_OK;
}

static BOOT_RAMFUNC HAL_StatusTypeDef flash_program_doubleword_ram(uint32_t address, uint64_t data) {
    HAL_StatusTypeDef status;

    status = flash_wait_ready_ram();
    if (status != HAL_OK) {
        return status;
    }

    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t*)address = (uint32_t)data;
    __ISB();
    *(volatile uint32_t*)(address + 4U) = (uint32_t)(data >> 32U);

    status = flash_wait_ready_ram();
    FLASH->CR &= ~FLASH_CR_PG;
    return status;
}

static uint32_t flash_bank_for_address(uint32_t address) {
#if defined(FLASH_OPTR_DBANK)
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U) {
        return (address < (FLASH_BASE + FLASH_BANK_SIZE)) ? FLASH_BANK_1 : FLASH_BANK_2;
    }
#endif
    (void)address;
    return FLASH_BANK_1;
}

static uint32_t flash_page_for_address(uint32_t address) {
#if defined(FLASH_OPTR_DBANK)
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U) {
        const uint32_t bank_base =
            (address < (FLASH_BASE + FLASH_BANK_SIZE)) ? FLASH_BASE : (FLASH_BASE + FLASH_BANK_SIZE);
        return (address - bank_base) / FLASH_PAGE_SIZE;
    }
#endif
    return (address - FLASH_BASE) / FLASH_PAGE_SIZE;
}

static uint32_t flash_bank_end_for_address(uint32_t address) {
#if defined(FLASH_OPTR_DBANK)
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U) {
        return (address < (FLASH_BASE + FLASH_BANK_SIZE)) ? (FLASH_BASE + FLASH_BANK_SIZE) : (FLASH_BASE + FLASH_SIZE);
    }
#endif
    (void)address;
    return FLASH_BASE + FLASH_SIZE;
}

static bool boot_erase_app_range(uint32_t start_addr, uint32_t end_addr) {
    uint32_t current = start_addr;

    while (current < end_addr) {
        uint32_t page_error = 0U;
        HAL_StatusTypeDef status;
        const uint32_t bank_end = flash_bank_end_for_address(current);
        const uint32_t chunk_end = (end_addr < bank_end) ? end_addr : bank_end;
        const uint32_t first_page = flash_page_for_address(current);
        const uint32_t page_count = (chunk_end - current + FLASH_PAGE_SIZE - 1U) / FLASH_PAGE_SIZE;

        (void)flash_bank_for_address(current);
        status = flash_erase_pages_ram(first_page, page_count, &page_error);
        boot_diag_note_flash_erase(status, page_error);
        if (status != HAL_OK) {
            return false;
        }

        current = chunk_end;
    }

    return true;
}

bool boot_prepare_flash(uint32_t firmware_size) {
    bool ok;
    const uint32_t erase_end = APP_START_ADDR + firmware_size;

    if ((firmware_size == 0U) || ((APP_START_ADDR + firmware_size) > APP_END_ADDR)) {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        boot_diag_note_flash_program(HAL_ERROR, FLASH->CR, 0U);
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    ok = boot_erase_app_range(APP_START_ADDR, erase_end);
    HAL_FLASH_Lock();

    return ok;
}

bool boot_write_chunk(uint32_t offset, const uint8_t* data, uint8_t len, uint32_t firmware_size) {
    uint32_t write_addr;
    uint32_t i;
    bool data_cache_enabled;
    bool is_last;

    if ((data == NULL) || (len == 0U)) {
        return false;
    }
    if (offset != get_boot_session()->received_size) {
        return false;
    }
    if ((offset + len) > firmware_size) {
        return false;
    }

    write_addr = APP_START_ADDR + offset;
    is_last = ((offset + len) == firmware_size);
    if (((write_addr & 0x7U) != 0U) || ((!is_last) && ((len & 0x7U) != 0U))) {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        boot_diag_note_flash_program(HAL_ERROR, FLASH->CR, 0U);
        return false;
    }
    data_cache_enabled = ((FLASH->ACR & FLASH_ACR_DCEN) != 0U);
    if (data_cache_enabled) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    i = 0U;
    while (i < len) {
        uint64_t dw;
        uint8_t copy = ((len - i) >= 8U) ? 8U : (uint8_t)(len - i);
        uint8_t j;
        uint32_t expected_low = 0xFFFFFFFFUL;
        uint32_t expected_high = 0xFFFFFFFFUL;
        uint32_t actual_low;
        uint32_t actual_high;
        for (j = 0U; j < copy; ++j) {
            uint32_t shift = (uint32_t)(8U * (j & 0x3U));
            if (j < 4U) {
                expected_low = (expected_low & ~(0xFFUL << shift)) | ((uint32_t)data[i + j] << shift);
            } else {
                expected_high = (expected_high & ~(0xFFUL << shift)) | ((uint32_t)data[i + j] << shift);
            }
        }
        dw = ((uint64_t)expected_high << 32U) | expected_low;
        if (flash_program_doubleword_ram(write_addr + i, dw) != HAL_OK) {
            boot_diag_note_flash_program(HAL_ERROR, FLASH->SR, 0U);
            boot_diag_note_flash_program(HAL_ERROR, FLASH->CR, copy);
            if (data_cache_enabled) {
                __HAL_FLASH_DATA_CACHE_ENABLE();
            }
            HAL_FLASH_Lock();
            return false;
        }
        actual_low = *(const volatile uint32_t*)(write_addr + i);
        actual_high = *(const volatile uint32_t*)(write_addr + i + 4U);
        if ((actual_low != expected_low) || (actual_high != expected_high)) {
            boot_diag_note_flash_program(HAL_ERROR, actual_low, (uint8_t)actual_high);
            if (data_cache_enabled) {
                __HAL_FLASH_DATA_CACHE_ENABLE();
            }
            HAL_FLASH_Lock();
            return false;
        }
        i += copy;
    }
    if (data_cache_enabled) {
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }
    HAL_FLASH_Lock();
    return true;
}

bool boot_metadata_read_transport(uint32_t* packed) {
    const BootMetadataRecord* const record =
        (const BootMetadataRecord*)BOOT_METADATA_ADDR;

    if ((packed == NULL) ||
        (record->magic != BOOT_METADATA_MAGIC) ||
        (record->magic_inverse != ~BOOT_METADATA_MAGIC) ||
        (record->packed_transport_inverse != ~record->packed_transport)) {
        return false;
    }

    *packed = record->packed_transport;
    return true;
}

bool boot_metadata_store_transport(uint32_t packed) {
    BootMetadataRecord record = {
        .magic = BOOT_METADATA_MAGIC,
        .packed_transport = packed,
        .packed_transport_inverse = ~packed,
        .magic_inverse = ~BOOT_METADATA_MAGIC
    };
    uint32_t existing = 0U;
    uint32_t page_error = 0U;
    uint64_t first_doubleword;
    uint64_t second_doubleword;

    if (boot_metadata_read_transport(&existing) && (existing == packed)) {
        return true;
    }

    first_doubleword =
        ((uint64_t)record.packed_transport << 32U) | (uint64_t)record.magic;
    second_doubleword =
        ((uint64_t)record.magic_inverse << 32U) |
        (uint64_t)record.packed_transport_inverse;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    if (flash_erase_pages_ram(
            flash_page_for_address(BOOT_METADATA_ADDR), 1U, &page_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }
    if ((flash_program_doubleword_ram(BOOT_METADATA_ADDR, first_doubleword) != HAL_OK) ||
        (flash_program_doubleword_ram(BOOT_METADATA_ADDR + 8U, second_doubleword) != HAL_OK)) {
        HAL_FLASH_Lock();
        return false;
    }
    HAL_FLASH_Lock();

    return boot_metadata_read_transport(&existing) && (existing == packed);
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    uint32_t out = crc;
    size_t i;

    for (i = 0U; i < len; ++i) {
        uint8_t bit;
        out ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)(-(int32_t)(out & 1U));
            out = (out >> 1U) ^ (CRC32_POLY & mask);
        }
    }
    return out;
}

void boot_jump_to_application(void) {
    uint32_t app_sp = *(const uint32_t*)APP_START_ADDR;
    uint32_t app_reset = *(const uint32_t*)(APP_START_ADDR + 4U);

    __disable_irq();
    HAL_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    SCB->VTOR = APP_START_ADDR;
    __asm volatile(
        "msr msp, %0\n"
        "cpsie i\n"
        "bx %1\n"
        :
        : "r"(app_sp), "r"(app_reset)
        : "memory"
    );
    __builtin_unreachable();
}
