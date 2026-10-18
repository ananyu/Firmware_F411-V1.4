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
 *
 * DATA frame integrity (v2):
 *   data[1..4]=offset, data[5]=len, data[6..6+len)=chunk,
 *   data[6+len..7+len)=CRC16-CCITT (init 0xFFFF, poly 0x1021, LE) over
 *   data[1..5+len) i.e. offset||len||chunk.
 * The atkp u8 additive checksum is only 1/256 strong; when the transparent
 * bridge desyncs under window flooding it can stitch raw wire bytes into a
 * payload that passes the u8 sum. The frame CRC16 pins every DATA chunk
 * (CRC fail -> ERR_OFFSET watermark -> host go-back-N resend, nothing is
 * programmed to flash).
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
/* Differential OTA: upload a patch instead of a full image. The patch is
 * staged in scratch (S7) and applied at DEND: slot0 is read XIP as the
 * baseline, the reconstructed image is written to slot1 and then goes
 * through the same TLV verify + boot_set_pending path as a full upload. */
#define OTA_SUB_DBEGIN  0x04u
#define OTA_SUB_DDATA   0x05u
#define OTA_SUB_DEND    0x06u

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

/* max chunk per DATA frame: ATKP_MAX_DATA_SIZE(30) - sub(1) - offset(4)
 * - len(1) - crc16(2) */
#define OTA_MAX_CHUNK       22u

/* DBEGIN payload: data[1..4]=patch_size, data[5..8]=old_size (baseline
 * bytes in slot0), data[9..12]=new_size (rebuilt image bytes), and
 * data[13..16]=CRC32 of the slot0 baseline [0, old_size). A baseline
 * mismatch is answered with OTA_ST_ERR_VERIFY and the host falls back to
 * a full-image upload. CRC32 only identifies the baseline; image
 * integrity is still enforced by the SHA256 TLV check at DEND. */
#define OTA_DBEGIN_LEN      17u

void otaInit(void);
void otaTask(void *param);
void otaHandlePacket(const atkp_t *p);

#endif /* __OTA_H */
