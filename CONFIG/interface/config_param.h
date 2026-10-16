#ifndef __CONFIG_PARAM_H
#define __CONFIG_PARAM_H
#include "sys.h"
#include <stdbool.h>
#include "kv_store.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * 配置参数驱动代码	
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
********************************************************************************/
												
typedef struct 
{
	float kp;
	float ki;
	float kd;
} pidInit_t;

typedef struct
{
	pidInit_t roll;
	pidInit_t pitch;	
	pidInit_t yaw;	
} pidParam_t;

typedef struct
{
	pidInit_t vx;
	pidInit_t vy;
	pidInit_t vz;
	
	pidInit_t x;
	pidInit_t y;
	pidInit_t z;
} pidParamPos_t;

typedef struct
{
	int16_t accZero[3];
	int16_t accGain[3];
} accBias_t;

typedef struct
{
	int16_t magZero[3];
} magBias_t;

typedef struct 
{
    int16_t rollDeciDegrees;
    int16_t pitchDeciDegrees;
    int16_t yawDeciDegrees;
} boardAlignment_t;


typedef struct	
{
	pidParam_t pidAngle;	/*角度PID*/	
	pidParam_t pidRate;		/*角速度PID*/	
	pidParamPos_t pidPos;	/*位置PID*/
	// accBias_t accBias;		/*加速度校准值*/
	// magBias_t magBias;		/*磁力计校准值*/
	float trimP;			/*pitch微调*/
	float trimR;			/*roll微调*/
	u16 thrustBase;			/*油门基础值*/
} configParam_t;

/* KV keys: one record per parameter field (namespace 0x00 = flight params).
 * KV records carry their own CRC16, so version/cksum fields are gone.
 * pidParamPos_t (72B) exceeds KV_VALUE_MAX_LEN (64B), so it is stored as
 * two records: velocity half (vx/vy/vz) and position half (x/y/z). */
#define KV_PID_ANGLE      (KV_NS_PARAM | 0x01U)
#define KV_PID_RATE       (KV_NS_PARAM | 0x02U)
#define KV_PID_POS_VEL    (KV_NS_PARAM | 0x03U)
#define KV_PID_POS_POS    (KV_NS_PARAM | 0x04U)
#define KV_TRIM_P         (KV_NS_PARAM | 0x05U)
#define KV_TRIM_R         (KV_NS_PARAM | 0x06U)
#define KV_THRUST_BASE    (KV_NS_PARAM | 0x07U)
 

extern configParam_t configParam;

void configParamInit(void);	/*参数配置初始化*/
void configParamTask(void* param);	/*参数配置任务*/
bool configParamTest(void);

void resetConfigParamPID(void);
void saveConfigRequest(void);
void configParamCommitIfSafe(void);

#endif /*__CONFIG_PARAM_H */

