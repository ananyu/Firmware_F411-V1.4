/*
 * ota.h - OTA download module (app side) for MiniFly STM32F411CE
 *
 * Receives a MCUBoot-signed image into slot 1 (S6 @ 0x08040000) over the
 * atkp link (radio or USART1 bridge), verifies header + SHA256 TLV against
 * the downloaded bytes, then marks the swap pending via boot_set_pending()
 * and resets so the MCUBoot bootloader performs the swap.
 *
 * Wire format (carried in DOWN_OTA frames, little-endian u32):
 *   data[0] = sub-command, remaining bytes are sub-command specific.
 * Response: UP_OTA frame, data[0]=sub, data[1]=status, data[2..5]=u32 value
 * (current rx offset for DATA, 0 for BEGIN/STATUS).
 */
#ifndef __OTA_H
#define __OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "atkp.h"

/* OTA sub-commands (data[0] of DOWN_OTA / UP_OTA frames) */
#define OTA_SUB_STATUS  0x00u
#define OTA_SUB_BEGIN   0x01u
#define OTA_SUB_DATA    0x02u
#define OTA_SUB_END     0x03u

/* response status codes (data[1] of UP_OTA) */
#define OTA_ST_OK          0u  /* accepted */
#define OTA_ST_ERR_ARMED   1u  /* refused: aircraft is armed */
#define OTA_ST_ERR_SIZE    2u  /* image size out of range */
#define OTA_ST_ERR_OFFSET  3u  /* data offset does not match expected */
#define OTA_ST_ERR_VERIFY  4u  /* size mismatch / header / hash / TLV bad */
#define OTA_ST_ERR_STATE   5u  /* command not allowed in current state */

/* max signed image size: slot1 minus swap-trailer reserve
 * (128K - 0x400; see CONFIG\interface\flash_map.h) */
#define OTA_MAX_IMAGE_SIZE  0x1FC00u

void otaInit(void);
void otaTask(void *param);
void otaHandlePacket(const atkp_t *p);

#endif /* __OTA_H */
