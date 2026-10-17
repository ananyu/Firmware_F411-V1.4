/*
 * ota_uart_bridge.h - USART1 (PA9/PA10, 115200 8N1) polling bridge to the
 * atkp frame layer. Feeds host-sent downlink frames into the shared atkp
 * rx queue and forwards OTA responses (any atkp frame passed to
 * otaUartBridgeSend) back out to the host. No interrupts are used, so the
 * dormant USART1_IRQHandler in uart1.c/usart.c stays out of the way.
 */
#ifndef __OTA_UART_BRIDGE_H
#define __OTA_UART_BRIDGE_H

#include <stdbool.h>
#include "atkp.h"

void otaUartBridgeTask(void *param);
bool otaUartBridgeSend(const atkp_t *p);

#endif /* __OTA_UART_BRIDGE_H */
