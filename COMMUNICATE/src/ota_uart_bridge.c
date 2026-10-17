/*
 * ota_uart_bridge.c - USART1 (PA9 TX / PA10 RX, 115200 8N1) polling bridge.
 *
 * RX: byte-level atkp frame parser (same wire format as radiolink); a valid
 * frame is pushed into the shared atkp rx queue and processed by
 * atkpRxAnlTask like a radio packet. TX: atkp frames queued via
 * otaUartBridgeSend() are forwarded to the host (used for OTA ACKs).
 *
 * Strictly polling: no USART interrupts, so this coexists with the dormant
 * USART1_IRQHandler in uart1.c / usart.c. The RX loop feeds the IWDG and
 * yields every 32 bytes so lower-priority tasks keep running during a
 * transfer.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ota_uart_bridge.h"
#include "atkp.h"
#include "debug_assert.h"
#include "watchdog.h"

#include "stm32f4xx.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_usart.h"

/*FreeRTOS*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define BR_TX_QUEUE_LEN  16
#define BR_YIELD_EVERY   32  /* received bytes between vTaskDelay(1) */

typedef enum
{
    BR_WAIT_B1 = 0,
    BR_WAIT_B2,
    BR_MSG_ID,
    BR_DATA_LEN,
    BR_DATA,
    BR_CKSUM
} bridgeRxState_e;

static struct
{
    bridgeRxState_e state;
    atkp_t frame;
    uint8_t index;
    uint8_t cksum;
} br;

static xQueueHandle brTxQueue;
static bool brUartInited = false;

static void bridgeUartInit(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

    gpio.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio);

    usart.USART_BaudRate = 115200;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
}

static void bridgeRxFeed(uint8_t c)
{
    switch (br.state) {
    case BR_WAIT_B1:
        if (c == DOWN_BYTE1) {
            br.cksum = c;
            br.state = BR_WAIT_B2;
        }
        break;
    case BR_WAIT_B2:
        if (c == DOWN_BYTE2) {
            br.cksum += c;
            br.state = BR_MSG_ID;
        } else {
            br.state = BR_WAIT_B1;
        }
        break;
    case BR_MSG_ID:
        br.frame.msgID = c;
        br.cksum += c;
        br.state = BR_DATA_LEN;
        break;
    case BR_DATA_LEN:
        if (c <= ATKP_MAX_DATA_SIZE) {
            br.frame.dataLen = c;
            br.cksum += c;
            br.index = 0;
            br.state = (c > 0) ? BR_DATA : BR_CKSUM;
        } else {
            br.state = BR_WAIT_B1;
        }
        break;
    case BR_DATA:
        br.frame.data[br.index++] = c;
        br.cksum += c;
        if (br.index == br.frame.dataLen) {
            br.state = BR_CKSUM;
        }
        break;
    case BR_CKSUM:
        if (br.cksum == c) {
            atkpReceivePacketBlocking(&br.frame);
        }
        br.state = BR_WAIT_B1;
        break;
    default:
        br.state = BR_WAIT_B1;
        break;
    }
}

static void bridgeSendFrame(const atkp_t *p)
{
    uint8_t buf[36];
    uint8_t cksum = 0;
    int n = 0;
    int i;

    buf[n++] = UP_BYTE1;
    buf[n++] = UP_BYTE2;
    buf[n++] = p->msgID;
    buf[n++] = p->dataLen;
    memcpy(&buf[n], p->data, p->dataLen);
    n += p->dataLen;
    for (i = 0; i < n; i++) {
        cksum += buf[i];
    }
    buf[n++] = cksum;

    for (i = 0; i < n; i++) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
        USART_SendData(USART1, buf[i]);
    }
}

bool otaUartBridgeSend(const atkp_t *p)
{
    if (brTxQueue == NULL) {
        return false;
    }
    return xQueueSend(brTxQueue, p, 0) == pdPASS;
}

void otaUartBridgeTask(void *param)
{
    portTickType lastRxTick;
    uint32_t rxSinceYield = 0;

    (void)param;

    brTxQueue = xQueueCreate(BR_TX_QUEUE_LEN, sizeof(atkp_t));
    ASSERT(brTxQueue);
    lastRxTick = xTaskGetTickCount();

    while (1) {
        if (!brUartInited) {
            bridgeUartInit();
            brUartInited = true;
        }

        /* drain TX queue */
        {
            atkp_t p;
            while (xQueueReceive(brTxQueue, &p, 0) == pdTRUE) {
                bridgeSendFrame(&p);
            }
        }

        /* RX poll */
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            uint8_t c = (uint8_t)USART_ReceiveData(USART1);
            bridgeRxFeed(c);
            lastRxTick = xTaskGetTickCount();
            rxSinceYield++;
            if (rxSinceYield >= BR_YIELD_EVERY) {
                rxSinceYield = 0;
                vTaskDelay(1);
            }
        } else {
            /* idle > 3 ms: sleep 1 tick to keep latency low when the
             * host starts streaming again */
            if ((portTickType)(xTaskGetTickCount() - lastRxTick) >= 3) {
                vTaskDelay(1);
                lastRxTick = xTaskGetTickCount();
            }
        }

        /* the idle hook may not get a chance to run during a transfer */
        watchdogReset();
    }
}
