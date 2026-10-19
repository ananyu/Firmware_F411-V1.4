#ifndef __PMW3901_H
#define __PMW3901_H

#include "sys.h"
#include <stdbool.h>
#include <stdint.h>

typedef __packed struct
{
    uint8_t motion;
    uint8_t observation;
    int16_t deltaX;
    int16_t deltaY;
    uint8_t squal;
    uint8_t rawDataSum;
    uint8_t maxRawData;
    uint8_t minRawData;
    uint16_t shutter;
} pmw3901Motion_t;

bool pmw3901Init(void);
uint8_t pmw3901ReadRegister(uint8_t address);
bool pmw3901WriteRegister(uint8_t address, uint8_t value);
bool pmw3901ReadMotion(pmw3901Motion_t *motion);

#endif /* __PMW3901_H */
