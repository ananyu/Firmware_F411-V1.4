/*
 * MCUBoot main entry for MiniFly STM32F411CE
 *
 * Replaces old USB-IAP main.c. Calls boot_go() and jumps to the app.
 * LED is used for minimal status indication (fast blink = boot ok,
 * slow blink = failure / no valid image).
 */

#include <string.h>
#include "stm32f4xx.h"
#include "bootutil/bootutil.h"
#include "bootutil/image.h"
#include "led.h"

/* App entry point typedef */
typedef void (*pFunction)(void);

static void jump_to_app(uint32_t addr)
{
    pFunction app_entry;
    uint32_t app_stack;

    /* Check stack pointer is in RAM */
    app_stack = *(volatile uint32_t *)addr;
    if ((app_stack & 0x2FFE0000) != 0x20000000) {
        return;
    }

    /* Disable all interrupts */
    {
        uint32_t i;
        for (i = 0; i < 8; i++) {
            NVIC->ICER[i] = 0xFFFFFFFF;
            NVIC->ICPR[i] = 0xFFFFFFFF;
        }
    }

    /* Set vector table offset (redundant if app sets it, but safe) */
    SCB->VTOR = addr;

    /* Get reset handler */
    app_entry = (pFunction)*(volatile uint32_t *)(addr + 4);

    /* Set MSP and jump */
    __set_MSP(app_stack);
    app_entry();
}

int main(void)
{
    struct boot_rsp rsp;
    fih_ret rc;

    /* Minimal hardware init for LED */
    ledInit();

    /* Run MCUBoot */
    rc = boot_go(&rsp);

    if (rc == 0 && rsp.br_hdr != NULL) {
        uint32_t app_addr = rsp.br_image_off + rsp.br_hdr->ih_hdr_size;

        /* Quick LED blink to indicate success */
        {
            uint32_t i;
            for (i = 0; i < 3; i++) {
                LED_BLUE_L = 0;
                for (volatile uint32_t d = 0; d < 200000; d++) {}
                LED_BLUE_L = 1;
                for (volatile uint32_t d = 0; d < 200000; d++) {}
            }
        }

        jump_to_app(app_addr);
    }

    /* Failure: slow blink forever */
    while (1) {
        LED_BLUE_L = !LED_BLUE_L;
        for (volatile uint32_t d = 0; d < 800000; d++) {}
    }
}
