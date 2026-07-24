#include <assert.h>
#include <math.h>

#include "ak8963_factory_adjustment.h"

int main(void)
{
    assert(fabsf(ak8963FactoryAdjustmentFactor(128U) - 1.0f) < 0.0001f);
    assert(fabsf(ak8963FactoryAdjustmentFactor(0U) - 0.5f) < 0.0001f);
    assert(fabsf(ak8963FactoryAdjustmentFactor(255U) - 1.49609375f) < 0.0001f);

    return 0;
}
