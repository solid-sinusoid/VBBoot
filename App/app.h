#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "main.h"
#include "stm32g4xx_hal.h"

#define DEFAULT_NODE_ID 0x444U
#define BL_CAN_STD_ID 0x444U
#define BL_ACK_ID_MASK 0x400U

#if defined(BOOTLOADER_FDCAN_ID_FORMAT_EXTENDED)
#define BOOTLOADER_FDCAN_ID_TYPE FDCAN_EXTENDED_ID
#define BOOTLOADER_FDCAN_ID_MASK 0x1FFFFFFFUL
#define BOOTLOADER_FDCAN_STD_FILTERS 0U
#define BOOTLOADER_FDCAN_EXT_FILTERS 1U
#else
#define BOOTLOADER_FDCAN_ID_TYPE FDCAN_STANDARD_ID
#define BOOTLOADER_FDCAN_ID_MASK 0x7FFUL
#define BOOTLOADER_FDCAN_STD_FILTERS 1U
#define BOOTLOADER_FDCAN_EXT_FILTERS 0U
#endif

#define APP_START_ADDR 0x08003000UL
#define APP_END_ADDR 0x0801F800UL
#define APP_MANIFEST_ADDR 0x0801F7C0UL
#define APP_MANIFEST_MAGIC 0x50414256UL
#define APP_MANIFEST_FORMAT_VERSION 1U
#define APP_MANIFEST_BOARD_ID 0x31444256UL
#define APP_BOOT_PROTOCOL_VERSION 1UL
#define BOOT_FLASH_PAGE_SIZE 0x800UL
#define CONFIG_EEPROM_I2C_DEV_ADDR 0x50U
#define CONFIG_EEPROM_MEM_ADDR 0x0000U
#define VBDRIVE_CONFIG_TYPE_ID 0x44AAABFFUL
#define BOOT_METADATA_ADDR 0x0801F800UL
#define BOOT_METADATA_MAGIC 0x424F4F54UL
#define BOOT_REQUEST_MAGIC 0xB00710ADUL
#define BOOT_LAUNCH_APP_MAGIC 0xA991CAFEUL
#define BOOT_RECOVERY_WINDOW_MS 1000UL

// Ensure that this matches `libvoltbro/voltbro/config/serial/serial.h` BaseConfigData EXACTLY
typedef struct __attribute__((packed)) {
    uint8_t was_configured;
    uint8_t node_id;
    uint8_t fdcan_nominal_baud;
    uint8_t fdcan_data_baud;
    uint32_t type_id;
} BootEepromConfigPrefix;
_Static_assert(sizeof(BootEepromConfigPrefix) == 8U, "Boot EEPROM config prefix must match BaseConfigData");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t board_id;
    uint32_t config_abi;
    uint32_t boot_protocol;
    uint32_t app_start;
    uint32_t app_end;
    uint32_t flags;
} BootApplicationManifest;
_Static_assert(sizeof(BootApplicationManifest) == 32U, "Application manifest must remain stable");
_Static_assert(APP_MANIFEST_ADDR + sizeof(BootApplicationManifest) <= APP_END_ADDR,
               "Application manifest must fit before recovery metadata");

// Ensure that this matches `libvoltbro/voltbro/config/serial/serial.h` FDCANNominalBaud EXACTLY
typedef enum {
    FDCAN_NOMINAL_KHZ62 = 0,
    FDCAN_NOMINAL_KHZ125 = 1,
    FDCAN_NOMINAL_KHZ250 = 2,
    FDCAN_NOMINAL_KHZ500 = 3,
    FDCAN_NOMINAL_KHZ1000 = 4
} BootFdcanNominalBaud;

// Ensure that this matches `libvoltbro/voltbro/config/serial/serial.h` FDCANDataBaud EXACTLY
typedef enum {
    FDCAN_DATA_KHZ1000 = 0,
    FDCAN_DATA_KHZ2000 = 1,
    FDCAN_DATA_KHZ4000 = 2,
    FDCAN_DATA_KHZ8000 = 3
} BootFdcanDataBaud;

#define BL_STATUS_DONE 0xD0U
#define BL_STATUS_ERR  0xE0U

typedef enum {
    BOOT_CMD_START = 1,
    BOOT_CMD_DATA = 2,
    BOOT_CMD_DONE = 3,
    BOOT_CMD_GET_ID = 5
} BootCommand;

typedef enum {
    BootStateIdle = 0,
    BootStateReceiving = 1,
    BootStateVerifyCrc = 2,
    BootStateError = 3
} BootState;

typedef struct {
    BootState state;
    uint32_t expected_size;
    uint32_t expected_crc32;
    uint32_t received_size;
    uint32_t running_crc32;
    bool flash_prepared;
} BootSession;

typedef struct {
    uint32_t can_id;
    uint8_t nominal_prescaler;
    uint8_t data_prescaler;
    bool fd_mode;
    bool bitrate_switch;
} BootTransportConfig;

extern FDCAN_HandleTypeDef hfdcan1;

BootSession* get_boot_session(void);
bool bootloader_start_requested(void);
bool boot_update_activity_seen(void);
void transport_config_load(void);
const BootTransportConfig* transport_config_get(void);
uint32_t boot_command_can_id(void);
uint32_t boot_ack_can_id(void);
void boot_diag_init(void);
void boot_diag_text(const char* text);
void boot_diag_note_reset_flags(void);
void boot_diag_note_transport_start(const BootTransportConfig* config);
void boot_diag_note_flash_erase(HAL_StatusTypeDef status, uint32_t page_error);
void boot_diag_note_flash_program(HAL_StatusTypeDef status, uint32_t address, uint8_t len);
void boot_diag_note_ack(uint8_t ack_status, HAL_StatusTypeDef tx_status);
void boot_diag_note_rx(uint8_t cmd, uint8_t len, uint32_t offset, uint8_t buf_len);
void boot_diag_note_loader_mode(bool app_valid, bool boot_requested);
void boot_diag_note_fdcan_status(const char* tag);

void configure_fdcan(FDCAN_HandleTypeDef* hfdcan, const BootTransportConfig* config);
void start_transport(void);
void transport_loop(void);

void boot_on_start(uint32_t size, uint32_t crc32);
bool boot_on_data(uint32_t offset, const uint8_t* data, uint8_t size);
bool boot_on_done(void);
void boot_send_ack(uint8_t status);
void boot_process_command(const uint8_t* p, size_t payload_size, void (*send_status)(uint8_t));
void boot_jump_to_application(void);

bool is_application_valid(void);
