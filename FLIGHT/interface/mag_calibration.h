#ifndef __MAG_CALIBRATION_H
#define __MAG_CALIBRATION_H

#include <stdbool.h>

typedef struct
{
    float offset[3];
    float scale[3];
} magCalibrationResult_t;

bool magCalibrationCompute(const float minimum[3], const float maximum[3],
                           magCalibrationResult_t *result);
void magCalibrationApply(const magCalibrationResult_t *calibration,
                         const float raw[3], float corrected[3]);

#endif
