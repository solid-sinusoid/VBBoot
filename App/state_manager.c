#include "app.h"

void configure_fdcan(FDCAN_HandleTypeDef* hfdcan, const BootTransportConfig* config) {
    hfdcan->Instance = FDCAN1;
    hfdcan->Init.ClockDivider = FDCAN_CLOCK_DIV2;
    hfdcan->Init.FrameFormat = FDCAN_FRAME_FD_BRS;
    hfdcan->Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan->Init.AutoRetransmission = ENABLE;
    hfdcan->Init.TransmitPause = DISABLE;
    hfdcan->Init.ProtocolException = DISABLE;
    hfdcan->Init.NominalSyncJumpWidth = 20;
    hfdcan->Init.NominalTimeSeg1 = 59;
    hfdcan->Init.NominalTimeSeg2 = 20;
    hfdcan->Init.DataSyncJumpWidth = 4;
    hfdcan->Init.DataTimeSeg1 = 5;
    hfdcan->Init.DataTimeSeg2 = 4;
    hfdcan->Init.StdFiltersNbr = BOOTLOADER_FDCAN_STD_FILTERS;
    hfdcan->Init.ExtFiltersNbr = BOOTLOADER_FDCAN_EXT_FILTERS;
    hfdcan->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    hfdcan->Init.NominalPrescaler = config->nominal_prescaler;
    hfdcan->Init.DataPrescaler = config->data_prescaler;

    if (HAL_FDCAN_Init(hfdcan) != HAL_OK) {
        Error_Handler();
    }

    FDCAN_FilterTypeDef filter = {0};
    filter.IdType = BOOTLOADER_FDCAN_ID_TYPE;
    filter.FilterIndex = 0;
    filter.FilterType = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1 = config->can_id & BOOTLOADER_FDCAN_ID_MASK;
    filter.FilterID2 = BOOTLOADER_FDCAN_ID_MASK;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &filter) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_FDCAN_ConfigGlobalFilter(
            hfdcan,
            FDCAN_REJECT,
            FDCAN_REJECT,
            FDCAN_REJECT_REMOTE,
            FDCAN_REJECT_REMOTE) != HAL_OK) {
        Error_Handler();
    }
}

