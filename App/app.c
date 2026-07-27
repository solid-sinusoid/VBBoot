#include "app.h"

#include "bootloader_runtime.h"
#include "i2c.h"

#define BOOT_CFG_NODE_ID_MASK       0x7FFUL
#define BOOT_CFG_NOMINAL_SHIFT      11U
#define BOOT_CFG_DATA_SHIFT         19U
#define BOOT_CFG_FD_MODE_BIT        27U
#define BOOT_CFG_BITRATE_SWITCH_BIT 28U

static BootSession boot_session = {
    .state = BootStateIdle,
    .expected_size = 0U,
    .expected_crc32 = 0U,
    .received_size = 0U,
    .running_crc32 = 0U,
    .flash_prepared = false
};

static BootTransportConfig g_transport_config = {
    .can_id = BL_CAN_STD_ID,
    .nominal_prescaler = 1U,
    .data_prescaler = 1U,
    .fd_mode = true,
    .bitrate_switch = true
};

static bool g_diag_ready = false;

static void boot_diag_putc(char ch) {
    if (!g_diag_ready) {
        return;
    }
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = (uint8_t)ch;
}

static void boot_diag_puts(const char* text) {
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        boot_diag_putc(*text++);
    }
}

static void boot_diag_hex_nibble(uint8_t value) {
    value &= 0x0FU;
    boot_diag_putc((char)((value < 10U) ? ('0' + value) : ('A' + (value - 10U))));
}

static void boot_diag_hex_u8(uint8_t value) {
    boot_diag_hex_nibble((uint8_t)(value >> 4U));
    boot_diag_hex_nibble(value);
}

static void boot_diag_hex_u32(uint32_t value) {
    uint8_t shift = 28U;

    while (1) {
        boot_diag_hex_nibble((uint8_t)(value >> shift));
        if (shift == 0U) {
            break;
        }
        shift = (uint8_t)(shift - 4U);
    }
}

static void boot_diag_space(void) {
    boot_diag_putc(' ');
}

static void boot_diag_crlf(void) {
    boot_diag_putc('\r');
    boot_diag_putc('\n');
}

static void boot_diag_tag(const char* tag) {
    boot_diag_puts(tag);
    boot_diag_space();
}

static void boot_diag_usart2_init(void) {
    const uint32_t brr = HAL_RCC_GetPCLK1Freq() / 19200U;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    GPIOA->MODER &= ~(GPIO_MODER_MODE2_Msk | GPIO_MODER_MODE3_Msk);
    GPIOA->MODER |= (0x2UL << GPIO_MODER_MODE2_Pos) | (0x2UL << GPIO_MODER_MODE3_Pos);
    GPIOA->OTYPER &= ~(GPIO_OTYPER_OT2 | GPIO_OTYPER_OT3);
    GPIOA->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED2_Msk | GPIO_OSPEEDR_OSPEED3_Msk);
    GPIOA->OSPEEDR |= (0x3UL << GPIO_OSPEEDR_OSPEED2_Pos) | (0x3UL << GPIO_OSPEEDR_OSPEED3_Pos);
    GPIOA->PUPDR &= ~(GPIO_PUPDR_PUPD2_Msk | GPIO_PUPDR_PUPD3_Msk);
    GPIOA->AFR[0] &= ~(GPIO_AFRL_AFSEL2_Msk | GPIO_AFRL_AFSEL3_Msk);
    GPIOA->AFR[0] |= (0x7UL << GPIO_AFRL_AFSEL2_Pos) | (0x7UL << GPIO_AFRL_AFSEL3_Pos);

    USART2->CR1 = 0U;
    USART2->CR2 = 0U;
    USART2->CR3 = 0U;
    USART2->BRR = brr;
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    while ((USART2->ISR & (USART_ISR_TEACK | USART_ISR_REACK)) != (USART_ISR_TEACK | USART_ISR_REACK)) {
    }
}

BootSession* get_boot_session(void) {
    return &boot_session;
}

void boot_diag_init(void) {
    boot_diag_usart2_init();
    g_diag_ready = true;
}

void boot_diag_text(const char* text) {
    boot_diag_puts(text);
    boot_diag_crlf();
}

void boot_diag_note_reset_flags(void) {
    boot_diag_tag("RST");
    boot_diag_hex_u32(RCC->CSR);
    boot_diag_crlf();
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

void boot_diag_note_transport_start(const BootTransportConfig* config) {
    boot_diag_tag("CFG");
    boot_diag_hex_u32(config->can_id);
    boot_diag_space();
    boot_diag_hex_u32(boot_ack_can_id());
    boot_diag_space();
    boot_diag_hex_u8(config->nominal_prescaler);
    boot_diag_space();
    boot_diag_hex_u8(config->data_prescaler);
    boot_diag_space();
    boot_diag_hex_u8(config->fd_mode ? 1U : 0U);
    boot_diag_space();
    boot_diag_hex_u8(config->bitrate_switch ? 1U : 0U);
    boot_diag_crlf();
}

void boot_diag_note_flash_erase(HAL_StatusTypeDef status, uint32_t page_error) {
    boot_diag_tag("FER");
    boot_diag_hex_u8((uint8_t)status);
    boot_diag_space();
    boot_diag_hex_u32(page_error);
    boot_diag_crlf();
}

void boot_diag_note_flash_program(HAL_StatusTypeDef status, uint32_t address, uint8_t len) {
    boot_diag_tag("FWR");
    boot_diag_hex_u8((uint8_t)status);
    boot_diag_space();
    boot_diag_hex_u32(address);
    boot_diag_space();
    boot_diag_hex_u8(len);
    boot_diag_space();
    boot_diag_hex_u32(get_boot_session()->received_size);
    boot_diag_crlf();
}

void boot_diag_note_ack(uint8_t ack_status, HAL_StatusTypeDef tx_status) {
    boot_diag_tag("ACK");
    boot_diag_hex_u8(ack_status);
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)tx_status);
    boot_diag_space();
    boot_diag_hex_u32(HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1));
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)get_boot_session()->state);
    boot_diag_space();
    boot_diag_hex_u32(get_boot_session()->received_size);
    boot_diag_crlf();
}

void boot_diag_note_rx(uint8_t cmd, uint8_t len, uint32_t offset, uint8_t buf_len) {
    boot_diag_tag("RX");
    boot_diag_hex_u8(cmd);
    boot_diag_space();
    boot_diag_hex_u8(len);
    boot_diag_space();
    boot_diag_hex_u32(offset);
    boot_diag_space();
    boot_diag_hex_u8(buf_len);
    boot_diag_crlf();
}

void boot_diag_note_loader_mode(bool app_valid, bool boot_requested) {
    boot_diag_tag("MODE");
    boot_diag_hex_u8(app_valid ? 1U : 0U);
    boot_diag_space();
    boot_diag_hex_u8(boot_requested ? 1U : 0U);
    boot_diag_crlf();
}

void boot_diag_note_fdcan_status(const char* tag) {
    FDCAN_ProtocolStatusTypeDef proto = {0};
    FDCAN_ErrorCountersTypeDef counters = {0};

    (void)HAL_FDCAN_GetProtocolStatus(&hfdcan1, &proto);
    (void)HAL_FDCAN_GetErrorCounters(&hfdcan1, &counters);

    boot_diag_tag("FDS");
    boot_diag_puts(tag);
    boot_diag_space();
    boot_diag_hex_u8(proto.BusOff ? 1U : 0U);
    boot_diag_space();
    boot_diag_hex_u8(proto.Warning ? 1U : 0U);
    boot_diag_space();
    boot_diag_hex_u8(proto.ErrorPassive ? 1U : 0U);
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)proto.LastErrorCode);
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)proto.DataLastErrorCode);
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)counters.TxErrorCnt);
    boot_diag_space();
    boot_diag_hex_u8((uint8_t)counters.RxErrorCnt);
    boot_diag_crlf();
}

static void boot_request_access_enable(void) {
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
}

static void boot_request_access_disable(void) {
    HAL_PWR_DisableBkUpAccess();
}

static void boot_request_unpack(uint32_t packed, BootTransportConfig* config) {
    config->can_id = packed & BOOT_CFG_NODE_ID_MASK;
    config->nominal_prescaler = (uint8_t)((packed >> BOOT_CFG_NOMINAL_SHIFT) & 0xFFU);
    config->data_prescaler = (uint8_t)((packed >> BOOT_CFG_DATA_SHIFT) & 0xFFU);
    config->fd_mode = ((packed >> BOOT_CFG_FD_MODE_BIT) & 0x1U) != 0U;
    config->bitrate_switch = ((packed >> BOOT_CFG_BITRATE_SWITCH_BIT) & 0x1U) != 0U;
}

static uint32_t boot_request_pack(const BootTransportConfig* config) {
    uint32_t packed = config->can_id & BOOT_CFG_NODE_ID_MASK;

    packed |= ((uint32_t)config->nominal_prescaler << BOOT_CFG_NOMINAL_SHIFT);
    packed |= ((uint32_t)config->data_prescaler << BOOT_CFG_DATA_SHIFT);
    if (config->fd_mode) {
        packed |= (1UL << BOOT_CFG_FD_MODE_BIT);
    }
    if (config->bitrate_switch) {
        packed |= (1UL << BOOT_CFG_BITRATE_SWITCH_BIT);
    }
    return packed;
}

static bool boot_request_read(BootTransportConfig* config, bool clear_request) {
    bool present;

    boot_request_access_enable();
    present = (TAMP->BKP0R == BOOT_REQUEST_MAGIC);
    if (present && (config != NULL)) {
        boot_request_unpack(TAMP->BKP1R, config);
    }
    if (present && clear_request) {
        TAMP->BKP0R = 0U;
        TAMP->BKP1R = 0U;
        TAMP->BKP2R = 0U;
    }
    boot_request_access_disable();
    return present;
}

static bool transport_config_is_valid(const BootTransportConfig* config) {
    return (config->can_id >= 1U) &&
           (config->can_id <= 0x7FFU) &&
           (config->nominal_prescaler > 0U) &&
           (config->data_prescaler > 0U) &&
           (config->fd_mode || (!config->bitrate_switch));
}

static void transport_config_load_defaults(void) {
    g_transport_config.can_id = (uint32_t)node_id_read();
    g_transport_config.nominal_prescaler = 1U;
    g_transport_config.data_prescaler = 1U;
    g_transport_config.fd_mode = true;
    g_transport_config.bitrate_switch = true;
}

void transport_config_load(void) {
    BootTransportConfig requested = {0};
    uint32_t stored_packed = 0U;

    transport_config_load_defaults();
    if (boot_request_read(&requested, true) && transport_config_is_valid(&requested)) {
        g_transport_config = requested;
        (void)boot_metadata_store_transport(boot_request_pack(&requested));
    } else if (boot_metadata_read_transport(&stored_packed)) {
        boot_request_unpack(stored_packed, &requested);
        if (transport_config_is_valid(&requested)) {
            g_transport_config = requested;
        }
    }
}

const BootTransportConfig* transport_config_get(void) {
    return &g_transport_config;
}

uint32_t boot_command_can_id(void) {
    return g_transport_config.can_id & 0x7FFU;
}

uint32_t boot_ack_can_id(void) {
    return (boot_command_can_id() | BL_ACK_ID_MASK) & 0x7FFU;
}

static bool is_valid_node_id(uint8_t node_id) {
    return (node_id >= 0x01U);
}

uint32_t node_id_read(void) {
    NodeIdRecord rec = {0};
    HAL_StatusTypeDef status;

    if (HAL_I2C_IsDeviceReady(&hi2c2, NODE_ID_EEPROM_I2C_DEV_ADDR << 1U, 2U, 100U) != HAL_OK) {
        return DEFAULT_NODE_ID;
    }

    status = HAL_I2C_Mem_Read(
        &hi2c2,
        NODE_ID_EEPROM_I2C_DEV_ADDR << 1U,
        NODE_ID_EEPROM_MEM_ADDR,
        I2C_MEMADD_SIZE_16BIT,
        (uint8_t*)&rec,
        (uint16_t)sizeof(rec),
        100U
    );

    if ((status == HAL_OK) && (rec.magic == NODE_ID_MAGIC) && is_valid_node_id(rec.node_id)) {
        return rec.node_id;
    }
    return DEFAULT_NODE_ID;
}

bool bootloader_start_requested(void) {
    return boot_request_read(NULL, false);
}

static bool boot_launch_app_marker_consume(void) {
    bool present;

    boot_request_access_enable();
    present = (TAMP->BKP2R == BOOT_LAUNCH_APP_MAGIC);
    if (present) {
        TAMP->BKP2R = 0U;
    }
    boot_request_access_disable();
    return present;
}

static void boot_launch_app_marker_set(void) {
    boot_request_access_enable();
    TAMP->BKP2R = BOOT_LAUNCH_APP_MAGIC;
    boot_request_access_disable();
}

void boot_on_start(uint32_t size, uint32_t crc32) {
    boot_session.expected_size = 0U;
    boot_session.expected_crc32 = 0U;
    boot_session.received_size = 0U;
    boot_session.running_crc32 = 0U;
    boot_session.flash_prepared = false;
    boot_session.state = BootStateIdle;
    boot_session.expected_size = size;
    boot_session.expected_crc32 = crc32;
    boot_session.running_crc32 = 0xFFFFFFFFUL;
    if (boot_prepare_flash(size)) {
        boot_session.flash_prepared = true;
        boot_session.state = BootStateReceiving;
    } else {
        boot_session.state = BootStateError;
    }
}

bool boot_on_data(uint32_t offset, const uint8_t* data, uint8_t size) {
    if ((boot_session.state != BootStateReceiving) || (!boot_session.flash_prepared)) {
        return false;
    }
    if (!boot_write_chunk(offset, data, size, boot_session.expected_size)) {
        boot_session.state = BootStateError;
        return false;
    }
    boot_session.running_crc32 = crc32_update(boot_session.running_crc32, data, size);
    boot_session.received_size += size;
    return true;
}

bool boot_on_done(void) {
    uint32_t final_crc;
    bool ok;

    if (boot_session.state != BootStateReceiving) {
        return false;
    }
    boot_session.state = BootStateVerifyCrc;
    final_crc = boot_session.running_crc32 ^ 0xFFFFFFFFUL;
    ok = (boot_session.received_size == boot_session.expected_size) && (final_crc == boot_session.expected_crc32);
    if (!ok) {
        boot_session.state = BootStateError;
        return false;
    }
    return true;
}

bool is_application_valid(void) {
    const uint32_t app_sp = *(const uint32_t*)APP_START_ADDR;
    const uint32_t app_reset = *(const uint32_t*)(APP_START_ADDR + 4U);
    const uint32_t app_nmi = *(const uint32_t*)(APP_START_ADDR + 8U);
    const uint32_t app_hardfault = *(const uint32_t*)(APP_START_ADDR + 12U);
    const bool stack_ok = (app_sp >= 0x20000000UL) && (app_sp <= 0x20008000UL) && ((app_sp & 0x7UL) == 0U);
    const bool reset_ok = (app_reset >= APP_START_ADDR) && (app_reset < APP_END_ADDR) && ((app_reset & 0x1UL) != 0U);
    const bool nmi_ok = (app_nmi >= APP_START_ADDR) && (app_nmi < APP_END_ADDR) && ((app_nmi & 0x1UL) != 0U);
    const bool hardfault_ok = (app_hardfault >= APP_START_ADDR) &&
                              (app_hardfault < APP_END_ADDR) &&
                              ((app_hardfault & 0x1UL) != 0U);

    if ((app_sp == 0xFFFFFFFFUL) ||
        (app_reset == 0xFFFFFFFFUL) ||
        (app_nmi == 0xFFFFFFFFUL) ||
        (app_hardfault == 0xFFFFFFFFUL)) {
        return false;
    }

    return stack_ok && reset_ok && nmi_ok && hardfault_ok;
}

void app(void) {
    const bool app_valid = is_application_valid();
    const bool boot_requested = bootloader_start_requested();
    const bool launch_app_after_clean_reset =
        (!boot_requested) && boot_launch_app_marker_consume();

    boot_diag_note_reset_flags();
    if (app_valid && !boot_requested && launch_app_after_clean_reset) {
        boot_diag_text("JAPP");
        boot_jump_to_application();
    }

    boot_diag_note_loader_mode(app_valid, boot_requested);
    if (app_valid && !boot_requested) {
        const uint32_t recovery_started_ms = HAL_GetTick();

        start_transport();
        while ((HAL_GetTick() - recovery_started_ms) < BOOT_RECOVERY_WINDOW_MS) {
            transport_loop();
            if (boot_update_activity_seen()) {
                boot_diag_text("RECOVERY");
                while (1) {
                    transport_loop();
                }
            }
        }

        /*
         * Do not jump after FDCAN has run. Request one clean reset and consume
         * this one-shot marker before starting the application on the next boot.
         */
        boot_diag_text("RAPP");
        boot_launch_app_marker_set();
        NVIC_SystemReset();
    } else {
        start_transport();
        while (1) {
            transport_loop();
        }
    }
}
