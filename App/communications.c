#include "app.h"

static uint8_t g_rx_data[64];
static uint32_t g_write_offset = 0U;
static bool g_start_part0_ready = false;
static uint32_t g_start_size = 0U;
static uint16_t g_start_crc_low = 0U;
static uint8_t g_data_buf[8];
static uint8_t g_data_buf_len = 0U;
static uint8_t g_uart_payload[64];
static uint8_t g_uart_len = 0U;
static uint8_t g_uart_pos = 0U;
static uint8_t g_uart_crc = 0U;
static uint8_t g_uart_state = 0U;
static bool g_boot_update_activity_seen = false;

bool boot_update_activity_seen(void) {
    return g_boot_update_activity_seen;
}

static uint32_t read_u32_le(const uint8_t* p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static size_t fdcan_dlc_to_len(uint32_t dlc) {
    switch (dlc) {
    case 0U:  return 0U;
    case 1U:  return 1U;
    case 2U:  return 2U;
    case 3U:  return 3U;
    case 4U:  return 4U;
    case 5U:  return 5U;
    case 6U:  return 6U;
    case 7U:  return 7U;
    case 8U:  return 8U;
    case 9U:  return 12U;
    case 10U: return 16U;
    case 11U: return 20U;
    case 12U: return 24U;
    case 13U: return 32U;
    case 14U: return 48U;
    case 15U: return 64U;
    default:  return 0U;
    }
}

static uint32_t fdcan_len_to_dlc(size_t len) {
    if (len <= 8U) {
        return (uint32_t)len;
    }
    return FDCAN_DLC_BYTES_8;
}

void boot_send_ack(uint8_t status) {
    FDCAN_TxHeaderTypeDef txh = {0};
    uint8_t payload[1];
    HAL_StatusTypeDef tx_status;
    payload[0] = status;
    txh.Identifier = boot_ack_can_id();
    txh.IdType = BOOTLOADER_FDCAN_ID_TYPE;
    txh.TxFrameType = FDCAN_DATA_FRAME;
    txh.DataLength = fdcan_len_to_dlc(1U);
    txh.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    txh.BitRateSwitch = FDCAN_BRS_ON;
    txh.FDFormat = FDCAN_FD_CAN;
    txh.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    txh.MessageMarker = 0U;
    tx_status = HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txh, payload);
    boot_diag_note_ack(status, tx_status);
    if (tx_status != HAL_OK) {
        boot_diag_note_fdcan_status("ack-fail");
    }
}

static void boot_uart_send_ack(uint8_t status) {
    while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) {
    }
    USART2->TDR = status;
}

void boot_process_command(const uint8_t* p, size_t payload_size, void (*send_status)(uint8_t)) {
    uint8_t cmd;

    if ((p == NULL) || (payload_size < 1U)) {
        return;
    }

    cmd = p[0];
    if ((cmd != BOOT_CMD_DATA) || (payload_size < 16U)) {
        boot_diag_note_rx(cmd, (uint8_t)payload_size, g_write_offset, g_data_buf_len);
    }
    switch ((BootCommand)cmd) {
    case BOOT_CMD_START: {
        uint8_t part;
        if (payload_size < 2U) {
            send_status(BL_STATUS_ERR);
            break;
        }
        part = p[1];
        if (part == 0U) {
            if (payload_size < 8U) {
                send_status(BL_STATUS_ERR);
                return;
            }
            g_start_size = read_u32_le(&p[2]);
            if ((g_start_size == 0U) ||
                (g_start_size > (APP_END_ADDR - APP_START_ADDR))) {
                send_status(BL_STATUS_ERR);
                return;
            }
            g_start_crc_low = (uint16_t)p[6] | ((uint16_t)p[7] << 8U);
            g_start_part0_ready = true;
            g_boot_update_activity_seen = true;
            g_write_offset = 0U;
            g_data_buf_len = 0U;
            send_status(BL_STATUS_DONE);
            break;
        }
        if (part == 1U) {
            uint32_t crc;
            uint16_t crc_high;
            if ((payload_size < 4U) || (!g_start_part0_ready)) {
                send_status(BL_STATUS_ERR);
                return;
            }
            crc_high = (uint16_t)p[2] | ((uint16_t)p[3] << 8U);
            crc = ((uint32_t)crc_high << 16U) | (uint32_t)g_start_crc_low;
            g_start_part0_ready = false;
            g_write_offset = 0U;
            g_data_buf_len = 0U;
            boot_on_start(g_start_size, crc);
            send_status((get_boot_session()->state == BootStateReceiving) ? BL_STATUS_DONE : BL_STATUS_ERR);
            break;
        }
        send_status(BL_STATUS_ERR);
        break;
    }
    case BOOT_CMD_DATA: {
        size_t idx;
        size_t remaining;
        bool ok = true;
        if (payload_size < 2U) {
            send_status(BL_STATUS_ERR);
            break;
        }
        idx = 1U;
        remaining = payload_size - 1U;
        if (get_boot_session()->state == BootStateReceiving) {
            const size_t buffered = (size_t)g_write_offset + (size_t)g_data_buf_len;
            const size_t expected = (size_t)get_boot_session()->expected_size;
            const size_t bytes_needed = (buffered < expected) ? (expected - buffered) : 0U;
            if (remaining > bytes_needed) {
                remaining = bytes_needed;
            }
        }
        while (remaining > 0U) {
            size_t space = 8U - g_data_buf_len;
            size_t take = (remaining < space) ? remaining : space;
            size_t j;
            for (j = 0U; j < take; ++j) {
                g_data_buf[g_data_buf_len + j] = p[idx + j];
            }
            g_data_buf_len = (uint8_t)(g_data_buf_len + take);
            idx += take;
            remaining -= take;
            if (g_data_buf_len == 8U) {
                if (!boot_on_data(g_write_offset, g_data_buf, 8U)) {
                    ok = false;
                    send_status(BL_STATUS_ERR);
                    boot_diag_note_fdcan_status("data-reject");
                    break;
                }
                g_write_offset += 8U;
                g_data_buf_len = 0U;
            }
        }
        if (ok) {
            send_status(BL_STATUS_DONE);
        }
        break;
    }
    case BOOT_CMD_DONE:
        if (g_data_buf_len > 0U) {
            BootSession* session = get_boot_session();
            if ((g_write_offset + g_data_buf_len) != session->expected_size) {
                send_status(BL_STATUS_ERR);
                break;
            }
            if (!boot_on_data(g_write_offset, g_data_buf, g_data_buf_len)) {
                send_status(BL_STATUS_ERR);
                boot_diag_note_fdcan_status("done-tail-reject");
                break;
            }
            g_write_offset += g_data_buf_len;
            g_data_buf_len = 0U;
        }
        if (boot_on_done()) {
            send_status(BL_STATUS_DONE);
            while (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) != 3U) {
            }
            boot_diag_text("JDONE");
            /*
             * Start the new application from reset state.  A direct jump after
             * using FDCAN can carry enabled or pending interrupts and peripheral
             * state across the bootloader/application boundary.
             */
            NVIC_SystemReset();
        } else {
            send_status(BL_STATUS_ERR);
            boot_diag_note_fdcan_status("done-reject");
        }
        break;
    default:
        send_status(BL_STATUS_ERR);
        break;
    }
}

static void uart_rx_byte(uint8_t byte) {
    switch (g_uart_state) {
    case 0U:
        if (byte == 0xA5U) {
            g_uart_state = 1U;
            g_uart_crc = 0U;
        }
        break;
    case 1U:
        if ((byte == 0U) || (byte > sizeof(g_uart_payload))) {
            g_uart_state = 0U;
            boot_uart_send_ack(BL_STATUS_ERR);
            break;
        }
        g_uart_len = byte;
        g_uart_pos = 0U;
        g_uart_crc = byte;
        g_uart_state = 2U;
        break;
    case 2U:
        g_uart_payload[g_uart_pos++] = byte;
        g_uart_crc ^= byte;
        if (g_uart_pos == g_uart_len) {
            g_uart_state = 3U;
        }
        break;
    default:
        if (byte == g_uart_crc) {
            boot_process_command(g_uart_payload, g_uart_len, boot_uart_send_ack);
        } else {
            boot_uart_send_ack(BL_STATUS_ERR);
        }
        g_uart_state = 0U;
        break;
    }
}

static void uart_transport_loop(void) {
    while ((USART2->ISR & USART_ISR_RXNE_RXFNE) != 0U) {
        uart_rx_byte((uint8_t)USART2->RDR);
    }
}


void start_transport(void) {
    const BootTransportConfig* config;

    transport_config_load();
    config = transport_config_get();
    boot_diag_note_transport_start(config);
    configure_fdcan(&hfdcan1, config);
    if (config->fd_mode && config->bitrate_switch) {
        (void)HAL_FDCAN_ConfigTxDelayCompensation(
            &hfdcan1,
            hfdcan1.Init.DataTimeSeg1 * hfdcan1.Init.DataPrescaler,
            0U
        );
        (void)HAL_FDCAN_EnableTxDelayCompensation(&hfdcan1);
    }
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        boot_diag_text("FDCAN_START_FAIL");
    }
    boot_diag_note_fdcan_status("start");
}

void transport_loop(void) {
    while (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) != 0U) {
        FDCAN_RxHeaderTypeDef rxh = {0};
        size_t rx_len;
        if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxh, g_rx_data) != HAL_OK) {
            boot_diag_text("RX_FETCH_FAIL");
            boot_diag_note_fdcan_status("rx-fetch");
            break;
        }
        if ((rxh.RxFrameType != FDCAN_DATA_FRAME) ||
            (rxh.IdType != BOOTLOADER_FDCAN_ID_TYPE) ||
            (rxh.Identifier != boot_command_can_id())) {
            continue;
        }
        rx_len = fdcan_dlc_to_len(rxh.DataLength);
        if (rx_len > sizeof(g_rx_data)) {
            continue;
        }
        boot_process_command(g_rx_data, rx_len, boot_send_ack);
    }
    uart_transport_loop();
}
