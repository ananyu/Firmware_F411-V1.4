#ifndef __SENSORS_H
#define __SENSORS_H
#include "stabilizer_types.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * 传感器控制代码	
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
********************************************************************************/

#define SENSORS_ENABLE_MAG_AK8963
//不带值的#define表示启用这个功能，注释掉表示不启用
#define SENSORS_ENABLE_PRESSURE_BMP280	/*气压计使用bmp280*/
#define SENSORS_I2CBUS	I2C1_DEV
#define BARO_UPDATE_RATE		RATE_50_HZ
#define SENSOR9_UPDATE_RATE   	RATE_500_HZ
#define SENSOR9_UPDATE_DT     	(1.0f/SENSOR9_UPDATE_RATE)

typedef struct
{
	Axis3f field;
	u32 timestamp;
	float magnitude;
	bool valid;
} magSample_t;

typedef struct
{
    Axis3f acc;        //加速度计
    Axis3f gyro;       //陀螺仪
} imuSample_t;

	
void sensorsTask(void *param);
void sensorsInit(void);			/*传感器初始化*/
bool sensorsTest(void);			/*传感器测试*/
bool sensorsAreCalibrated(void);	/*传感器数据校准*/
void sensorsAcquire(sensorData_t *sensors, const u32 tick);/*获取传感器数据*/
void getSensorRawData(Axis3i16* acc, Axis3i16* gyro, Axis3i16* mag);
bool getIsMPU9250Present(void);
bool getIsBaroPresent(void);
bool sensorsIsMagPresent(void);
void sensorsMagCalibrationStart(void);
bool sensorsMagCalibrationFinish(void);
void sensorsMagCalibrationCancel(void);

/* 单独测量传感器数据 */

bool sensorsReadMag(magSample_t *sample);
bool sensorsReadBaro(baro_t *baro);
bool sensorsReadImuSample(imuSample_t *sample);

#endif //__SENSORS_H
