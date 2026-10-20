/*
 * ota.c - OTA download state machine (app side) for MiniFly STM32F411CE.
 *
 * Flow: BEGIN (erase slot1, arm) -> DATA* (word-programmed staged writes,
 * sequential offsets only) -> END (flush, re-hash from flash, check image
 * header + SHA256 TLV, boot_set_pending(0), ACK, reset into MCUBoot swap).
 *
 * Runs in otaTask (prio 4); packets are forwarded from atkpReceiveAnl()
 * via a small queue so flash erase/program never blocks the rx analysis
 * task. ACKs go out both on the radio link and on the USART1 bridge.
 */
#include <string.h>
#include "freertos_static.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "ota.h"
#include "radiolink.h"
#include "atkp.h"
#include "flash_map.h"
#include "sysflash.h"
#include "flash_map_backend.h"
#include "bootutil/bootutil_public.h"
#include "bootutil/image.h"
/* bootutil_public.h defines ASSERT as C-library assert(); the FW uses the
 * assertFail() variant from debug_assert.h. Pin the FW definition here
 * (the header guard then makes the later pull via FreeRTOSConfig.h a no-op). */
#undef ASSERT
#include "debug_assert.h"
#include "ota_uart_bridge.h"

#include "stm32f4xx.h"
#include "stm32f4xx_flash.h"
#include "stm32f4xx_iwdg.h"
#include <tinycrypt/sha256.h>
#include <tinycrypt/constants.h>

/*FreeRTOS*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Queue must absorb a full sender window (W=8 frames) plus link jitter
 * without dropping; otaHandlePacket() drops on overflow and the host
 * re-syncs via the ACK watermark, but deep queueing keeps the pipeline
 * fed. 24 x 32 B = 768 B RAM. */
#define OTA_QUEUE_LEN   24

typedef enum
{
    OTA_IDLE = 0,
    OTA_DOWNLOAD,   /* full-image download into slot1 */
    OTA_PATCH,      /* differential patch download into scratch */
    OTA_BUSY        /* DEND accepted: rebuilding slot1, link silent by design */
} otaState_e;

static otaState_e otaState = OTA_IDLE;
static uint32_t otaTotalSize;   /* full: image size / diff: new image size */
static uint32_t otaRxOffset;    /* bytes accepted so far (ACKed to host) */
static uint32_t otaProgOffset;  /* bytes already committed to flash */
static uint32_t otaPatchSize;   /* diff: patch bytes staged in scratch */
static uint32_t otaOldSize;     /* diff: baseline span in slot0 */
static uint32_t otaRxBuf;       /* write target base (slot1 or scratch) */
static uint8_t  otaTail[4];     /* staged bytes waiting for a full word */
static uint8_t  otaTailLen;
static xQueueHandle otaQueue;
static bool otaIsInit = false;

static uint32_t otaReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC16-CCITT (init 0xFFFF, poly 0x1021), bitwise: 22-byte chunk -> ~350
 * cycles, negligible next to flash programming. */
static uint16_t otaCrc16(const uint8_t *p, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        int i;
        crc ^= (uint16_t)(*p++) << 8;
        for (i = 0; i < 8; i++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* CRC32 (zlib-compatible: poly 0xEDB88320 reflected, init/final 0xFFFFFFFF)
 * used to fingerprint the slot0 baseline at DBEGIN. Bitwise: 110 KB XIP
 * baseline -> ~10 ms @ 100 MHz, negligible next to the sector erase. */
static uint32_t otaCrc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        int i;
        crc ^= *p++;
        for (i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* IWDG relax: prescaler 32 -> 256, reload 0xFFF => ~32 s window.
 * A 128 KB sector erase can take up to ~4 s; the stock 150 ms budget
 * would reset the MCU in the middle of BEGIN. */
static void otaWatchdogRelax(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_256);
    IWDG_SetReload(0xFFF);
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Disable);
    IWDG_ReloadCounter();
}

/* Bounded flash word-program. The stock FLASH_ProgramWord() waits on BSY via
 * FLASH_WaitForLastOperation() with NO timeout: a stuck program cycle spins
 * otaTask forever, the idle-task watchdog feeder never runs, and ~32 s later
 * the IWDG resets the board mid-rebuild (observed: applyPatch stopped at
 * out=18988 and the aircraft went permanently offline). This variant:
 *  - masks interrupts for the ~16 us program cycle (an ISR fetching code from
 *    flash mid-cycle can abort the operation with PGSERR on F4),
 *  - clears stale error flags, forces x32 parallelism, and TIMES OUT on BSY
 *    instead of spinning forever,
 *  - checks the error flags and read-back-verifies the word,
 *  - retries a few times and logs addr + FLASH->SR on final failure so a
 *    stuck offset is diagnosable over the debug UART. */
#define OTA_PROG_TIMEOUT  50000u  /* loop count: far above the 16 us/word spec */
#define OTA_PROG_RETRIES  3

#define OTA_PROG_ERR_FLAGS (FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | \
                            FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | \
                            FLASH_FLAG_PGSERR)

static bool otaProgramWordAttempt(uint32_t addr, uint32_t word)
{
    uint32_t primask;
    uint32_t timeout;
    bool timedOut;

    primask = __get_PRIMASK();
    __disable_irq();

    FLASH_Unlock();
    FLASH_ClearFlag(OTA_PROG_ERR_FLAGS);
    FLASH->CR &= ~CR_PSIZE_MASK;
    FLASH->CR |= FLASH_PSIZE_WORD;
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = word;

    timeout = OTA_PROG_TIMEOUT;
    while (((FLASH->SR & FLASH_FLAG_BSY) != 0) && (timeout-- != 0)) {
    }
    timedOut = ((FLASH->SR & FLASH_FLAG_BSY) != 0);

    FLASH->CR &= ~FLASH_CR_PG;
    FLASH_Lock();

    if (primask == 0) {
        __enable_irq();
    }

    if (timedOut) {
        return false; /* controller wedged: leave SR alone */
    }
    if ((FLASH->SR & OTA_PROG_ERR_FLAGS) != 0) {
        FLASH_ClearFlag(OTA_PROG_ERR_FLAGS);
        return false;
    }
    return *(volatile uint32_t *)addr == word;
}

static bool otaProgramWord(uint32_t addr, uint32_t word)
{
    int retry;
    for (retry = 0; retry < OTA_PROG_RETRIES; retry++) {
        if (otaProgramWordAttempt(addr, word)) {
            return true;
        }
        IWDG_ReloadCounter(); /* keep the dog fed across retries */
    }
    printf("[OTA] program fail @0x%08lx word=0x%08lx SR=0x%08lx\n",
           (unsigned long)addr, (unsigned long)word,
           (unsigned long)FLASH->SR);
    return false;
}

/* Append received bytes to the active target (otaRxBuf), programming full
 * 32-bit words. */
static int otaFlashAppend(const uint8_t *data, uint32_t len)
{
    while (len--) {
        otaTail[otaTailLen++] = *data++;
        otaRxOffset++;
        if (otaTailLen == 4) {
            uint32_t word;
            memcpy(&word, otaTail, 4);
            if (!otaProgramWord(otaRxBuf + otaProgOffset, word)) {
                return -1;
            }
            otaProgOffset += 4;
            otaTailLen = 0;
        }
    }
    return 0;
}

/* Flush the staged tail, padding the last word with 0xFF (erased value).
 * Well-formed images are always word-sized, so this is only a safety net. */
static int otaFlashFlush(void)
{
    if (otaTailLen > 0) {
        uint32_t word = 0xFFFFFFFF;
        memcpy(&word, otaTail, otaTailLen);
        if (!otaProgramWord(otaRxBuf + otaProgOffset, word)) {
            return -1;
        }
        otaProgOffset += 4;
        otaTailLen = 0;
    }
    return 0;
}

static int otaComputeHash(uint32_t len, uint8_t out[IMAGE_HASH_LEN])
{
    struct tc_sha256_state_struct ctx;
    const uint8_t *src = (const uint8_t *)FM_SLOT1_BASE;
    uint32_t chunk;

    if (tc_sha256_init(&ctx) != TC_CRYPTO_SUCCESS) {
        return -1;
    }
    while (len > 0) {
        chunk = (len > 512) ? 512 : len;
        if (tc_sha256_update(&ctx, src, chunk) != TC_CRYPTO_SUCCESS) {
            return -1;
        }
        src += chunk;
        len -= chunk;
    }
    return (tc_sha256_final(out, &ctx) == TC_CRYPTO_SUCCESS) ? 0 : -1;
}

/* Verify the downloaded image in slot1:
 * header magic/hdr_size, then locate the SHA256 TLV and compare it against
 * a hash recomputed over header+image ([0, hdr_size + img_size)).
 */
static int otaVerifyImage(void)
{
    struct image_header hdr;
    struct image_tlv_info info;
    uint8_t digest[IMAGE_HASH_LEN];
    uint8_t ref[IMAGE_HASH_LEN];
    uint32_t img_end;
    uint32_t tlv_base;
    uint32_t off;
    uint32_t end;

    memcpy(&hdr, (const void *)FM_SLOT1_BASE, sizeof(hdr));
    if (hdr.ih_magic != IMAGE_MAGIC) {
        return -1;
    }
    if (hdr.ih_hdr_size != FM_IMG_HDR_SIZE) {
        return -2;
    }
    img_end = (uint32_t)hdr.ih_hdr_size + hdr.ih_img_size;
    if (img_end > otaRxOffset) {
        return -3;
    }
    if (otaComputeHash(img_end, digest) != 0) {
        return -4;
    }

    /* TLV area starts right after the image; matches MCUBoot's
     * BOOT_TLV_OFF: ih_hdr_size + ih_img_size (no padding). */
    tlv_base = FM_SLOT1_BASE + hdr.ih_hdr_size + hdr.ih_img_size;
    if (tlv_base + sizeof(info) > FM_SLOT1_BASE + otaRxOffset) {
        return -5;
    }
    memcpy(&info, (const void *)tlv_base, sizeof(info));
    if (info.it_magic != IMAGE_TLV_INFO_MAGIC) {
        return -6;
    }
    off = tlv_base + sizeof(info);
    end = tlv_base + info.it_tlv_tot;
    if (end > FM_SLOT1_BASE + otaRxOffset) {
        return -7;
    }
    while (off + sizeof(struct image_tlv) <= end) {
        struct image_tlv tlv;
        memcpy(&tlv, (const void *)off, sizeof(tlv));
        if (tlv.it_type == IMAGE_TLV_SHA256 && tlv.it_len == IMAGE_HASH_LEN) {
            memcpy(ref, (const void *)(off + sizeof(tlv)), IMAGE_HASH_LEN);
            return (memcmp(digest, ref, IMAGE_HASH_LEN) == 0) ? 0 : -8;
        }
        off += sizeof(tlv) + ((tlv.it_len + 3u) & ~3u);
    }
    return -9;
}

static void otaSendAck(uint8_t sub, uint8_t status, uint32_t value)
{
    atkp_t p;

    p.msgID = UP_OTA;
    p.dataLen = 6;
    p.data[0] = sub;
    p.data[1] = status;
    p.data[2] = (uint8_t)(value);
    p.data[3] = (uint8_t)(value >> 8);
    p.data[4] = (uint8_t)(value >> 16);
    p.data[5] = (uint8_t)(value >> 24);

    radiolinkSendPacketBlocking(&p);
    otaUartBridgeSend(&p);
}

static void otaProcessBegin(const atkp_t *p)
{
    uint32_t total;
    const struct flash_area *fa = NULL;

    if (p->dataLen < 5) {
        otaSendAck(OTA_SUB_BEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    total = otaReadU32(&p->data[1]);

    if (atkpIsFlightUnlocked()) {
        otaSendAck(OTA_SUB_BEGIN, OTA_ST_ERR_ARMED, 0);
        return;
    }
    if (total < IMAGE_HEADER_SIZE || total > OTA_MAX_IMAGE_SIZE) {
        otaSendAck(OTA_SUB_BEGIN, OTA_ST_ERR_SIZE, 0);
        return;
    }

    otaWatchdogRelax();

    if (flash_area_open(FLASH_AREA_IMAGE_0_SECONDARY, &fa) != 0) {
        otaSendAck(OTA_SUB_BEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    if (flash_area_erase(fa, 0, fa->fa_size) != 0) {
        flash_area_close(fa);
        otaSendAck(OTA_SUB_BEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    flash_area_close(fa);
    IWDG_ReloadCounter();

    otaTotalSize = total;
    otaRxOffset = 0;
    otaProgOffset = 0;
    otaTailLen = 0;
    otaRxBuf = FM_SLOT1_BASE;
    otaState = OTA_DOWNLOAD;
    otaSendAck(OTA_SUB_BEGIN, OTA_ST_OK, 0);
}

static void otaProcessData(const atkp_t *p)
{
    uint32_t offset;
    uint32_t len;
    uint16_t rxcrc;
    uint16_t calcrc;

    /* serves both the full (slot1) and diff (scratch) download states;
     * otaRxBuf selects the target. */
    if (otaState == OTA_IDLE || p->dataLen < 8) {
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }
    offset = otaReadU32(&p->data[1]);
    len = p->data[5];
    if (len == 0 || len > OTA_MAX_CHUNK ||
        len + 8u > (uint32_t)p->dataLen) {
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }
    /* Frame CRC16 over offset||len||chunk: the atkp u8 sum lets parser-
     * desync garbage through 1/256 of the time; without this check the
     * stitched wire bytes get programmed into slot1 silently (observed:
     * 5 corrupted regions / 38 bad bytes in one 109 KB transfer). */
    calcrc = otaCrc16(&p->data[1], 5u + len);
    rxcrc = (uint16_t)p->data[6u + len] |
            ((uint16_t)p->data[7u + len] << 8);
    if (calcrc != rxcrc) {
        /* Do NOT program anything; answer with the watermark so the host
         * go-back-N resends from otaRxOffset. */
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_OFFSET, otaRxOffset);
        return;
    }
    if (offset != otaRxOffset) {
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_OFFSET, otaRxOffset);
        return;
    }
    if (len > otaTotalSize - otaRxOffset) {
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_SIZE, otaRxOffset);
        return;
    }

    if (otaFlashAppend(&p->data[6], len) != 0) {
        otaState = OTA_IDLE; /* flash fault: force a fresh BEGIN */
        otaSendAck(OTA_SUB_DATA, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }
    IWDG_ReloadCounter();
    otaSendAck(OTA_SUB_DATA, OTA_ST_OK, otaRxOffset);
}

static void otaProcessEnd(const atkp_t *p)
{
    (void)p;
    if (otaState != OTA_DOWNLOAD) {
        otaSendAck(OTA_SUB_END, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }
    if (otaRxOffset != otaTotalSize) {
        otaSendAck(OTA_SUB_END, OTA_ST_ERR_VERIFY, otaRxOffset);
        return;
    }
    if (otaFlashFlush() != 0 || otaVerifyImage() != 0) {
        otaState = OTA_IDLE;
        otaSendAck(OTA_SUB_END, OTA_ST_ERR_VERIFY, otaRxOffset);
        return;
    }
    if (boot_set_pending(0) != 0) {
        /* image is still valid in slot1: allow END retry */
        otaSendAck(OTA_SUB_END, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }

    otaSendAck(OTA_SUB_END, OTA_ST_OK, otaTotalSize);
    vTaskDelay(500); /* let the ACK drain before reset */
    NVIC_SystemReset();
}

/* ---- Differential OTA -------------------------------------------------
 * Patch is streamed into scratch (S7) via DBEGIN/DDATA, then applied at
 * DEND: slot0 is read XIP as the baseline and the rebuilt image is written
 * to slot1, followed by the same TLV verify + boot_set_pending path as a
 * full upload. Patch opcodes (little-endian fields):
 *   0x01 u32 src_off, u16 n : COPY n bytes from slot0[src_off]
 *   0x02 u8 n, n bytes      : LIT n literal bytes (1..255)
 *   0xFF                    : END
 * Integrity chain: per-frame CRC16 on the wire, baseline CRC32 at DBEGIN,
 * and the signed image's SHA256 TLV over the rebuilt result at DEND.
 */

#define OTA_PATCH_OP_COPY  0x01u
#define OTA_PATCH_OP_LIT   0x02u
#define OTA_PATCH_OP_END   0xFFu

static int otaPatchByte(const uint8_t **pp, const uint8_t *pend)
{
    if (*pp >= pend) {
        return -1;
    }
    return *(*pp)++;
}

static int otaPatchWord(const uint8_t **pp, const uint8_t *pend, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int c = otaPatchByte(pp, pend);
        if (c < 0) {
            return -1;
        }
        v |= ((uint32_t)c) << (8 * i);
    }
    *out = v;
    return 0;
}

/* Interpret the scratch-resident patch stream: read slot0 XIP, stage words,
 * program into slot1. Memory-safe: every COPY is bounds-checked against the
 * declared baseline size, malformed opcodes abort before any further flash
 * writes; a failed run simply leaves slot1 garbage that the next BEGIN (or
 * full-upload fallback) re-erases. */
static int otaApplyPatch(void)
{
    const uint8_t *pp = (const uint8_t *)FM_SCRATCH_BASE;
    const uint8_t *pend = pp + otaPatchSize;
    uint32_t out = 0;
    int rc = 0;

    while (pp < pend) {
        int op = otaPatchByte(&pp, pend);
        /* 每条指令都喂狗：单条指令最大工作量约 64KB 编程(~0.3s)，远在
         * 28s 窗口内。原先只在 COPY 分支按 1KB 喂狗，长 LIT 串重建时
         * 会断喂狗被 IWDG 复位。 */
        IWDG_ReloadCounter();
        if (op == OTA_PATCH_OP_END) {
            break;
        } else if (op == OTA_PATCH_OP_COPY) {
            uint32_t src;
            uint32_t n;
            if (otaPatchWord(&pp, pend, &src) != 0) {
                rc = -1;
                break;
            }
            n = 0;
            {
                int i;
                for (i = 0; i < 2; i++) {
                    int c = otaPatchByte(&pp, pend);
                    if (c < 0) {
                        rc = -1;
                        break;
                    }
                    n |= ((uint32_t)c) << (8 * i);
                }
            }
            if (rc != 0 || n == 0 || src > otaOldSize || n > otaOldSize - src) {
                rc = -1;
                break;
            }
            {
                const uint8_t *sp = (const uint8_t *)(FM_SLOT0_BASE + src);
                while (n--) {
                    otaTail[otaTailLen++] = *sp++;
                    if (otaTailLen == 4) {
                        uint32_t word;
                        memcpy(&word, otaTail, 4);
                        if (!otaProgramWord(FM_SLOT1_BASE + out, word)) {
                            rc = -1;
                            break;
                        }
                        out += 4;
                        otaTailLen = 0;
                    }
                    if ((out & 0x3FFu) == 0 && otaTailLen == 0) {
                        IWDG_ReloadCounter();
                    }
                }
            }
            if (rc != 0) {
                break;
            }
        } else if (op == OTA_PATCH_OP_LIT) {
            int n = otaPatchByte(&pp, pend);
            if (n <= 0 || pp + n > pend) {
                rc = -1;
                break;
            }
            while (n--) {
                otaTail[otaTailLen++] = *pp++;
                if (otaTailLen == 4) {
                    uint32_t word;
                    memcpy(&word, otaTail, 4);
                    if (!otaProgramWord(FM_SLOT1_BASE + out, word)) {
                        rc = -1;
                        break;
                    }
                    out += 4;
                    otaTailLen = 0;
                }
            }
            if (rc != 0) {
                break;
            }
        } else {
            rc = -1;
            break;
        }
    }

    if (rc == 0) {
        /* flush the staged tail (pad with erased value) */
        if (otaTailLen > 0) {
            uint32_t word = 0xFFFFFFFF;
            memcpy(&word, otaTail, otaTailLen);
            if (!otaProgramWord(FM_SLOT1_BASE + out, word)) {
                rc = -1;
            } else {
                out += 4;
                otaTailLen = 0;
            }
        }
    }

    if (rc == 0 && out != otaTotalSize) {
        rc = -1;
    }
    return rc;
}

/* DBEGIN payload: patch_size, old_size, new_size, baseline CRC32 (all LE).
 * Refuses early when the running image does not match what the patch was
 * generated against -- the host then falls back to a full upload. */
static void otaProcessDBegin(const atkp_t *p)
{
    uint32_t patch_size;
    uint32_t old_size;
    uint32_t new_size;
    uint32_t old_crc;
    uint32_t crc;
    const struct flash_area *fa = NULL;

    if (p->dataLen < OTA_DBEGIN_LEN) {
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    patch_size = otaReadU32(&p->data[1]);
    old_size = otaReadU32(&p->data[5]);
    new_size = otaReadU32(&p->data[9]);
    old_crc = otaReadU32(&p->data[13]);

    if (atkpIsFlightUnlocked()) {
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_ARMED, 0);
        return;
    }
    if (patch_size < 1 || patch_size > OTA_MAX_IMAGE_SIZE ||
        old_size > OTA_MAX_IMAGE_SIZE ||
        new_size < IMAGE_HEADER_SIZE || new_size > OTA_MAX_IMAGE_SIZE) {
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_SIZE, 0);
        return;
    }

    otaWatchdogRelax();

    if (flash_area_open(FLASH_AREA_IMAGE_SCRATCH, &fa) != 0) {
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    if (flash_area_erase(fa, 0, fa->fa_size) != 0) {
        flash_area_close(fa);
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_STATE, 0);
        return;
    }
    flash_area_close(fa);
    IWDG_ReloadCounter();

    /* Baseline fingerprint: XIP-read the running image and compare. */
    crc = otaCrc32((const uint8_t *)FM_SLOT0_BASE, old_size);
    if (crc != old_crc) {
        otaSendAck(OTA_SUB_DBEGIN, OTA_ST_ERR_VERIFY, 0);
        return;
    }

    otaPatchSize = patch_size;
    otaOldSize = old_size;
    otaTotalSize = new_size;
    otaRxOffset = 0;
    otaProgOffset = 0;
    otaTailLen = 0;
    otaRxBuf = FM_SCRATCH_BASE;
    otaState = OTA_PATCH;
    otaSendAck(OTA_SUB_DBEGIN, OTA_ST_OK, 0);
}

static void otaProcessDEnd(const atkp_t *p)
{
    (void)p;
    if (otaState != OTA_PATCH) {
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_STATE, otaRxOffset);
        return;
    }
    if (otaRxOffset != otaPatchSize) {
        otaState = OTA_IDLE;
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_VERIFY, otaRxOffset);
        return;
    }
    if (otaFlashFlush() != 0) {
        otaState = OTA_IDLE;
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_VERIFY, otaRxOffset);
        return;
    }

    /* ACK first: the rebuild below (erase + apply + verify) keeps the link
     * silent for several seconds -- longer than the host's DEND timeout --
     * and a late ACK then collides with the host's retries.  Reply up
     * front, mark the state busy so retried DEND frames are refused, and
     * let the host watch for the reboot instead.  A failure after this
     * point is reported via a follow-up error ACK; the host treats a
     * missing reset (STATUS answered without any silence) the same way. */
    otaState = OTA_BUSY;
    otaSendAck(OTA_SUB_DEND, OTA_ST_OK, otaTotalSize);

    /* The running slot1 image (or whatever a previous attempt left there)
     * must be erased before reconstruction: word-programming can only flip
     * bits to 0, writing over a programmed image fails on the first
     * differing word. */
    {
        const struct flash_area *fa = NULL;
        if (flash_area_open(FLASH_AREA_IMAGE_0_SECONDARY, &fa) != 0) {
            otaState = OTA_IDLE;
            IWDG_ReloadCounter();
            otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_STATE, 0);
            return;
        }
        if (flash_area_erase(fa, 0, fa->fa_size) != 0) {
            flash_area_close(fa);
            otaState = OTA_IDLE;
            IWDG_ReloadCounter();
            otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_STATE, 0);
            return;
        }
        flash_area_close(fa);
        IWDG_ReloadCounter();
    }
    /* Rebuild: slot1 output, staging buffer reused from the flush above. */
    otaProgOffset = 0;
    if (otaApplyPatch() != 0) {
        otaState = OTA_IDLE;
        IWDG_ReloadCounter();
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_VERIFY, 0);
        return;
    }
    otaRxOffset = otaTotalSize;
    otaProgOffset = otaTotalSize;
    if (otaVerifyImage() != 0) {
        otaState = OTA_IDLE;
        IWDG_ReloadCounter();
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_VERIFY, 0);
        return;
    }
    if (boot_set_pending(0) != 0) {
        /* image is still valid in slot1: allow a manual DEND retry */
        otaState = OTA_PATCH;
        otaSendAck(OTA_SUB_DEND, OTA_ST_ERR_STATE, 0);
        return;
    }

    vTaskDelay(500); /* let the ACK drain before reset */
    NVIC_SystemReset();
}

static void otaProcess(const atkp_t *p)
{
    if (p->dataLen < 1) {
        return;
    }
    switch (p->data[0]) {
    case OTA_SUB_BEGIN:
        otaProcessBegin(p);
        break;
    case OTA_SUB_DATA:
        otaProcessData(p);
        break;
    case OTA_SUB_END:
        otaProcessEnd(p);
        break;
    case OTA_SUB_DBEGIN:
        otaProcessDBegin(p);
        break;
    case OTA_SUB_DDATA:
        otaProcessData(p);  /* same frame format, target = scratch */
        break;
    case OTA_SUB_DEND:
        otaProcessDEnd(p);
        break;
    case OTA_SUB_STATUS:
    default:
        otaSendAck(OTA_SUB_STATUS, OTA_ST_OK,
                   (otaState != OTA_IDLE) ? otaRxOffset : 0);
        break;
    }
}

void otaTask(void *param)
{
    atkp_t p;
    (void)param;

    while (1) {
        if (xQueueReceive(otaQueue, &p, portMAX_DELAY) == pdTRUE) {
            otaProcess(&p);
        }
    }
}

void otaHandlePacket(const atkp_t *p)
{
    if (!otaIsInit) {
        return;
    }
    /* Queue full: drop; the host re-syncs via STATUS / ACK timeout. */
    (void)xQueueSend(otaQueue, p, 0);
}

void otaInit(void)
{
    if (otaIsInit) {
        return;
    }
    DEFINE_STATIC_QUEUE(ota, OTA_QUEUE_LEN, sizeof(atkp_t));
    otaQueue = xQueueCreateStatic(OTA_QUEUE_LEN, sizeof(atkp_t), sto_ota, &qbuf_ota);
    if (otaQueue == NULL) {
        for (;;) {
        }
    }
    otaState = OTA_IDLE;
    otaIsInit = true;
}
