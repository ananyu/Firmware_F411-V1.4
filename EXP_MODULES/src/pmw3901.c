#include "pmw3901.h"
#include "delay.h"
#include "spi.h"

#include "FreeRTOS.h"
#include "task.h"

/* PMW3901 chip-select and product identifiers. */
#define PMW3901_NCS_PIN       PAout(8)
#define PMW3901_PRODUCT_ID    0x49u
#define PMW3901_INVERSE_ID    0xB8u

static bool spiReady = false;

static bool pmw3901ReadRegisterValue(uint8_t address, uint8_t *value)
{
    bool result = false;
    uint8_t response = 0;

    if (value == NULL)
    {
        return false;
    }

    address &= (uint8_t)~0x80u;

    spiBeginTransaction();
    PMW3901_NCS_PIN = 0;

    delay_us(50);
    if (spiExchange(1, &address, &address))
    {
        delay_us(500);
        result = spiExchange(1, &response, &response);
    }

    PMW3901_NCS_PIN = 1;
    spiEndTransaction();
    delay_us(200);

    if (result)
    {
        *value = response;
    }

    return result;
}

uint8_t pmw3901ReadRegister(uint8_t address)
{
    uint8_t value = 0;

    (void)pmw3901ReadRegisterValue(address, &value);
    return value;
}

bool pmw3901WriteRegister(uint8_t address, uint8_t value)
{
    bool result;

    address |= 0x80u;

    spiBeginTransaction();
    PMW3901_NCS_PIN = 0;

    delay_us(50);
    result = spiExchange(1, &address, &address);
    if (result)
    {
        delay_us(50);
        result = spiExchange(1, &value, &value);
    }
    delay_us(50);

    PMW3901_NCS_PIN = 1;
    spiEndTransaction();
    delay_us(200);

    return result;
}

static bool pmw3901ReadMotionSample(pmw3901Motion_t *motion)
{
    pmw3901Motion_t sample;
    uint8_t address = 0x16u;
    uint8_t tx[sizeof(sample)] = {0};
    bool result;

    if (motion == NULL)
    {
        return false;
    }

    spiBeginTransaction();
    PMW3901_NCS_PIN = 0;

    delay_us(50);
    result = spiExchange(1, &address, &address);
    if (result)
    {
        delay_us(50);
        result = spiExchange(sizeof(sample), tx, (uint8_t *)&sample);
    }
    delay_us(50);

    PMW3901_NCS_PIN = 1;
    spiEndTransaction();
    delay_us(50);

    if (result)
    {
        sample.shutter = (uint16_t)((sample.shutter >> 8) |
                                    (sample.shutter << 8));
        *motion = sample;
    }

    return result;
}

bool pmw3901ReadMotion(pmw3901Motion_t *motion)
{
    return pmw3901ReadMotionSample(motion);
}

static bool pmw3901ConfigureRegisters(void)
{
#define PMW3901_WRITE(reg, value) \
    do { if (!pmw3901WriteRegister((reg), (value))) return false; } while (0)

    PMW3901_WRITE(0x7F, 0x00);
    PMW3901_WRITE(0x61, 0xAD);
    PMW3901_WRITE(0x7F, 0x03);
    PMW3901_WRITE(0x40, 0x00);
    PMW3901_WRITE(0x7F, 0x05);
    PMW3901_WRITE(0x41, 0xB3);
    PMW3901_WRITE(0x43, 0xF1);
    PMW3901_WRITE(0x45, 0x14);
    PMW3901_WRITE(0x5B, 0x32);
    PMW3901_WRITE(0x5F, 0x34);
    PMW3901_WRITE(0x7B, 0x08);
    PMW3901_WRITE(0x7F, 0x06);
    PMW3901_WRITE(0x44, 0x1B);
    PMW3901_WRITE(0x40, 0xBF);
    PMW3901_WRITE(0x4E, 0x3F);
    PMW3901_WRITE(0x7F, 0x08);
    PMW3901_WRITE(0x65, 0x20);
    PMW3901_WRITE(0x6A, 0x18);
    PMW3901_WRITE(0x7F, 0x09);
    PMW3901_WRITE(0x4F, 0xAF);
    PMW3901_WRITE(0x5F, 0x40);
    PMW3901_WRITE(0x48, 0x80);
    PMW3901_WRITE(0x49, 0x80);
    PMW3901_WRITE(0x57, 0x77);
    PMW3901_WRITE(0x60, 0x78);
    PMW3901_WRITE(0x61, 0x78);
    PMW3901_WRITE(0x62, 0x08);
    PMW3901_WRITE(0x63, 0x50);
    PMW3901_WRITE(0x7F, 0x0A);
    PMW3901_WRITE(0x45, 0x60);
    PMW3901_WRITE(0x7F, 0x00);
    PMW3901_WRITE(0x4D, 0x11);
    PMW3901_WRITE(0x55, 0x80);
    PMW3901_WRITE(0x74, 0x1F);
    PMW3901_WRITE(0x75, 0x1F);
    PMW3901_WRITE(0x4A, 0x78);
    PMW3901_WRITE(0x4B, 0x78);
    PMW3901_WRITE(0x44, 0x08);
    PMW3901_WRITE(0x45, 0x50);
    PMW3901_WRITE(0x64, 0xFF);
    PMW3901_WRITE(0x65, 0x1F);
    PMW3901_WRITE(0x7F, 0x14);
    PMW3901_WRITE(0x65, 0x67);
    PMW3901_WRITE(0x66, 0x08);
    PMW3901_WRITE(0x63, 0x70);
    PMW3901_WRITE(0x7F, 0x15);
    PMW3901_WRITE(0x48, 0x48);
    PMW3901_WRITE(0x7F, 0x07);
    PMW3901_WRITE(0x41, 0x0D);
    PMW3901_WRITE(0x43, 0x14);
    PMW3901_WRITE(0x4B, 0x0E);
    PMW3901_WRITE(0x45, 0x0F);
    PMW3901_WRITE(0x44, 0x42);
    PMW3901_WRITE(0x4C, 0x80);
    PMW3901_WRITE(0x7F, 0x10);
    PMW3901_WRITE(0x5B, 0x02);
    PMW3901_WRITE(0x7F, 0x07);
    PMW3901_WRITE(0x40, 0x41);
    PMW3901_WRITE(0x70, 0x00);

    vTaskDelay(10);

    PMW3901_WRITE(0x32, 0x44);
    PMW3901_WRITE(0x7F, 0x07);
    PMW3901_WRITE(0x40, 0x40);
    PMW3901_WRITE(0x7F, 0x06);
    PMW3901_WRITE(0x62, 0xF0);
    PMW3901_WRITE(0x63, 0x00);
    PMW3901_WRITE(0x7F, 0x0D);
    PMW3901_WRITE(0x48, 0xC0);
    PMW3901_WRITE(0x6F, 0xD5);
    PMW3901_WRITE(0x7F, 0x00);
    PMW3901_WRITE(0x5B, 0xA0);
    PMW3901_WRITE(0x4E, 0xA8);
    PMW3901_WRITE(0x5A, 0x50);
    PMW3901_WRITE(0x40, 0x80);

#undef PMW3901_WRITE
    return true;
}

bool pmw3901Init(void)
{
    GPIO_InitTypeDef gpio;
    uint8_t chipId;
    uint8_t inverseId;

    if (!spiReady)
    {
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

        gpio.GPIO_Pin = GPIO_Pin_8;
        gpio.GPIO_Mode = GPIO_Mode_OUT;
        gpio.GPIO_OType = GPIO_OType_PP;
        gpio.GPIO_Speed = GPIO_Speed_25MHz;
        gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
        GPIO_Init(GPIOA, &gpio);

        PMW3901_NCS_PIN = 1;
        spi2Init();
        spiReady = true;
    }

    if (!pmw3901ReadRegisterValue(0x00, &chipId) ||
        !pmw3901ReadRegisterValue(0x5F, &inverseId) ||
        chipId != PMW3901_PRODUCT_ID ||
        inverseId != PMW3901_INVERSE_ID)
    {
        return false;
    }

    if (!pmw3901WriteRegister(0x3A, 0x5A))
    {
        return false;
    }
    vTaskDelay(5);

    if (!pmw3901ConfigureRegisters())
    {
        return false;
    }
    vTaskDelay(5);

    return true;
}
