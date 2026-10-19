#include <stdbool.h>
#include <string.h>
#include "math.h"
#include "config.h"
#include "config_param.h"
#include "watchdog.h"

/*FreeRTOS相关头文件*/
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

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


configParam_t configParam;

static configParam_t configParamDefault=
{
	.pidAngle=	/*角度PID*/
	{	
		.roll=
		{
			.kp=8.0,
			.ki=0.0,
			.kd=0.0,
		},
		.pitch=
		{
			.kp=8.0,
			.ki=0.0,
			.kd=0.0,
		},
		.yaw=
		{
			.kp=20.0,
			.ki=0.0,
			.kd=1.5,
		},
	},	
	.pidRate=	/*角速度PID*/
	{	
		.roll=
		{
			.kp=300.0,
			.ki=0.0,
			.kd=6.5,
		},
		.pitch=
		{
			.kp=300.0,
			.ki=0.0,
			.kd=6.5,
		},
		.yaw=
		{
			.kp=200.0,
			.ki=18.5,
			.kd=0.0,
		},
	},	
	.pidPos=	/*位置PID*/
	{	
		.vx=
		{
			.kp=4.5,
			.ki=0.0,
			.kd=0.0,
		},
		.vy=
		{
			.kp=4.5,
			.ki=0.0,
			.kd=0.0,
		},
		.vz=
		{
			.kp=100.0,
			.ki=150.0,
			.kd=10.0,
		},
		
		.x=
		{
			.kp=4.0,
			.ki=0.0,
			.kd=0.6,
		},
		.y=
		{
			.kp=4.0,
			.ki=0.0,
			.kd=0.6,
		},
		.z=
		{
			.kp=6.0,
			.ki=0.0,
			.kd=4.5,
		},
	},
	
	.trimP = 0.f,	/*pitch微调*/
	.trimR = 0.f,	/*roll微调*/
	.thrustBase=34000,	/*定高油门基础值*/
	.magCalibration =
	{
		.offset = {0.0f, 0.0f, 0.0f},
		.scale = {1.0f, 1.0f, 1.0f},
		.fieldMagnitude = 0.0f,
		.formatVersion = 1,
		.valid = false,
	},
};

static bool isInit = false;
static volatile bool savePending = false;

static SemaphoreHandle_t  xSemaphore = NULL;

/* Read one KV record into buf; fall back to the built-in default when the
 * key is absent (fresh KV) or the stored length no longer matches. */
static bool kvReadWithDefault(u16 key, void *buf, u16 expectedLen, const void *defaultVal)
{
	u16 len = expectedLen;

	if (kv_get(key, buf, &len) && len == expectedLen)
	{
		return true;
	}
	memcpy(buf, defaultVal, expectedLen);
	return false;
}

void configParamInit(void)	/*参数配置初始化*/
{
	bool ok;

	if(isInit) return;

	ok = kv_init();

	if (ok)
	{
		ok &= kvReadWithDefault(KV_PID_ANGLE,   &configParam.pidAngle,   sizeof(configParam.pidAngle),   &configParamDefault.pidAngle);
		ok &= kvReadWithDefault(KV_PID_RATE,    &configParam.pidRate,    sizeof(configParam.pidRate),    &configParamDefault.pidRate);
		/* pidPos (72B) is split into velocity/position halves; both halves
		 * are contiguous pidInit_t[3] blocks inside the struct. */
		ok &= kvReadWithDefault(KV_PID_POS_VEL, &configParam.pidPos.vx,  sizeof(pidInit_t) * 3U,         &configParamDefault.pidPos.vx);
		ok &= kvReadWithDefault(KV_PID_POS_POS, &configParam.pidPos.x,   sizeof(pidInit_t) * 3U,         &configParamDefault.pidPos.x);
		ok &= kvReadWithDefault(KV_TRIM_P,      &configParam.trimP,      sizeof(configParam.trimP),      &configParamDefault.trimP);
		ok &= kvReadWithDefault(KV_TRIM_R,      &configParam.trimR,      sizeof(configParam.trimR),      &configParamDefault.trimR);
		ok &= kvReadWithDefault(KV_THRUST_BASE, &configParam.thrustBase, sizeof(configParam.thrustBase), &configParamDefault.thrustBase);
		ok &= kvReadWithDefault(KV_MAG_CALIBRATION, &configParam.magCalibration, sizeof(configParam.magCalibration), &configParamDefault.magCalibration);
	}

	if (!ok)
	{
		/* KV mount failed or some key unreadable: fly on defaults. */
		memcpy((u8 *)&configParam, (u8 *)&configParamDefault, sizeof(configParam));
	}

	xSemaphore = xSemaphoreCreateBinary();

	isInit=true;
}

void configParamTask(void* param)
{
	while(1)
	{
		xSemaphoreTake(xSemaphore, portMAX_DELAY);
		if (!savePending)
		{
			continue;
		}

		watchdogReset();	/* GC may erase a 16KB sector (~300ms) */

		/* per-key append; KV dedup skips fields that did not change */
		kv_set(KV_PID_ANGLE,   &configParam.pidAngle,   sizeof(configParam.pidAngle));
		kv_set(KV_PID_RATE,    &configParam.pidRate,    sizeof(configParam.pidRate));
		kv_set(KV_PID_POS_VEL, &configParam.pidPos.vx,  sizeof(pidInit_t) * 3U);
		kv_set(KV_PID_POS_POS, &configParam.pidPos.x,   sizeof(pidInit_t) * 3U);
		kv_set(KV_TRIM_P,      &configParam.trimP,      sizeof(configParam.trimP));
		kv_set(KV_TRIM_R,      &configParam.trimR,      sizeof(configParam.trimR));
		kv_set(KV_THRUST_BASE, &configParam.thrustBase, sizeof(configParam.thrustBase));
		kv_set(KV_MAG_CALIBRATION, &configParam.magCalibration, sizeof(configParam.magCalibration));

		savePending = false;
	}
}

bool configParamTest(void)
{
	return isInit;
}

void resetConfigParamPID(void)
{
	configParam.pidAngle = configParamDefault.pidAngle;
	configParam.pidRate = configParamDefault.pidRate;
	configParam.pidPos = configParamDefault.pidPos;
	saveConfigRequest();
}

void saveConfigRequest(void)
{
	savePending = true;
}

void configParamCommitIfSafe(void)
{
	if (savePending && xSemaphore != NULL)
	{
		xSemaphoreGive(xSemaphore);
	}
}
