#include <assert.h>
#include <math.h>

#include "mag_calibration.h"

int main(void)
{
    const float minimum[3] = {-20.0f, -40.0f, -35.0f};
    const float maximum[3] = {40.0f, 0.0f, 45.0f};
    magCalibrationResult_t result;

    assert(magCalibrationCompute(minimum, maximum, &result));
    assert(fabsf(result.offset[0] - 10.0f) < 0.0001f);
    assert(fabsf(result.offset[1] + 20.0f) < 0.0001f);
    assert(fabsf(result.offset[2] - 5.0f) < 0.0001f);
    assert(fabsf(result.scale[0] - 1.0f) < 0.0001f);
    assert(fabsf(result.scale[1] - 1.5f) < 0.0001f);
    assert(fabsf(result.scale[2] - 0.75f) < 0.0001f);

    return 0;
}
