#ifndef AK8963_FACTORY_ADJUSTMENT_H
#define AK8963_FACTORY_ADJUSTMENT_H

#include <stdint.h>

static inline float ak8963FactoryAdjustmentFactor(uint8_t asa)
{
    return ((float)asa - 128.0f) / 256.0f + 1.0f;
}

#endif
