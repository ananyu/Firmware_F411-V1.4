#include <math.h>
#include <stddef.h>

#include "mag_calibration.h"

bool magCalibrationCompute(const float minimum[3], const float maximum[3],
                           magCalibrationResult_t *result)
{
    float halfSpan[3];
    float averageHalfSpan = 0.0f;

    if (minimum == NULL || maximum == NULL || result == NULL)
    {
        return false;
    }

    for (unsigned int axis = 0; axis < 3; axis++)
    {
        halfSpan[axis] = 0.5f * (maximum[axis] - minimum[axis]);
        if (!isfinite(minimum[axis]) || !isfinite(maximum[axis]) ||
            halfSpan[axis] <= 0.0f)
        {
            return false;
        }

        result->offset[axis] = 0.5f * (maximum[axis] + minimum[axis]);
        averageHalfSpan += halfSpan[axis];
    }

    averageHalfSpan /= 3.0f;

    for (unsigned int axis = 0; axis < 3; axis++)
    {
        result->scale[axis] = averageHalfSpan / halfSpan[axis];
    }

    return true;
}
