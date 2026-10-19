#include "string.h"
#include "stdbool.h"
#include "optical_flow.h"
#include "pmw3901.h"
#include "config_param.h"
#include "commander.h"
#include "delay.h"
#include "maths.h"
#include "state_estimator.h"

#include "filter.h"
#include "arm_math.h"

/*FreeRTOS相关头文件*/
#include "FreeRTOS.h"
#include "task.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * 光流模块驱动代码	
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
 *
 * 修改说明:
 * 版本V1.3 增加光流数据结构体opFlow_t，用于存放光流各项数据，方便用户调试。
********************************************************************************/

#define OPTICAL_POWER_ENABLE	PBout(0)

#define RESOLUTION			(0.2131946f)/*1m高度下 1个像素对应的位移，单位cm*/
#define OULIER_LIMIT 		(100)		/*光流像素输出限幅*/
#define VEL_LIMIT			(150.f)		/*光流速度限幅*/

#define VEL_LPF_FILTER			/*低通滤波*/
//#define AVERAGE_FILTER		/*均值滤波*/

static bool isInit = false;
static u8 outlierCount = 0;			/*数据不可用计数*/

opFlow_t opFlow;	/*光流*/


TaskHandle_t opFlowTaskHandle = NULL;
pmw3901Motion_t currentMotion;

static void resetOpFlowData(void)
{
	for(u8 i=0; i<2; i++)
	{
		opFlow.pixSum[i] = 0;
		opFlow.pixComp[i] = 0;
		opFlow.pixValid[i] = 0;
		opFlow.pixValidLast[i] = 0;
	}
}

/*光流任务函数*/
void opticalFlowTask(void *param)
{	
	static u16 count = 0;	
	u32 lastWakeTime = getSysTickCnt();
		
	opFlow.isOpFlowOk = true;
	
	while(1) 
	{
		vTaskDelayUntil(&lastWakeTime, 10);		/*100Hz 10ms周期延时*/
		
		bool motionReadOk = pmw3901ReadMotion(&currentMotion);

		if (!motionReadOk ||
		    (currentMotion.minRawData == 0 && currentMotion.maxRawData == 0))
		{
			if(count++ > 100 && opFlow.isOpFlowOk == true)
			{
				count = 0;
				opFlow.isOpFlowOk = false;		/*光流出错*/
				vTaskSuspend(opFlowTaskHandle);	/*挂起光流任务*/
			}		
		}else
		{
			count = 0;
		}

		if (!motionReadOk)
		{
			continue;
		}
		/*连续2帧之间的像素变化，根据实际安装方向调整 (pitch:x)  (roll:y)*/
		int16_t pixelDx = currentMotion.deltaY;
		int16_t pixelDy = -currentMotion.deltaX;

		if (ABS(pixelDx) < OULIER_LIMIT && ABS(pixelDy) < OULIER_LIMIT) 
		{
			opFlow.pixSum[X] += pixelDx;
			opFlow.pixSum[Y] += pixelDy;
		}else 
		{
			outlierCount++;
		}
	}
}

#ifdef AVERAGE_FILTER

#define GROUP		2
#define FILTER_NUM	3

/*限幅均值滤波法*/
void velFilter(float* in, float* out)
{	
	static u8 i=0;
	static float filter_buf[GROUP][FILTER_NUM] = {0.0};
	double filter_sum[GROUP] = {0.0};		
	
	for(u8 j=0;j<GROUP;j++)
	{
		if(filter_buf[j][i] == 0.0f)
		{
			filter_buf[j][i] = in[j];
			out[j] = in[j];			
		} else 
		{			
			if(fabs(in[j]) < VEL_LIMIT)
			{
				filter_buf[j][i] = in[j];
			}
			for(u8 cnt=0;cnt<FILTER_NUM;cnt++)
			{
				filter_sum[j] += filter_buf[j][cnt];
			}
			out[j]=filter_sum[j] /FILTER_NUM;
		}
	}
	if(++i >= FILTER_NUM)	i = 0;
}
#endif

bool getOpFlowData(state_t *state, float dt)
{
	static u8 cnt = 0;
	float height = 0.01f * getFusedHeight();/*读取高度信息 单位m*/
	
	if(opFlow.isOpFlowOk && height<4.0f)	/*4m范围内，光流可用*/
	{
		cnt= 0;
		opFlow.isDataValid = true;
		
		float coeff = RESOLUTION * height;
		float tanRoll = tanf(state->attitude.roll * DEG2RAD);
		float tanPitch = tanf(state->attitude.pitch * DEG2RAD);
		
		opFlow.pixComp[X] = 480.f * tanPitch;	/*像素补偿，负方向*/
		opFlow.pixComp[Y] = 480.f * tanRoll;
		opFlow.pixValid[X] = (opFlow.pixSum[X] + opFlow.pixComp[X]);	/*实际输出像素*/
		opFlow.pixValid[Y] = (opFlow.pixSum[Y] + opFlow.pixComp[Y]);		
		
		if(height < 0.05f)	/*光流测量范围大于5cm*/
		{
			coeff = 0.0f;
		}
		opFlow.deltaPos[X] = coeff * (opFlow.pixValid[X] - opFlow.pixValidLast[X]);	/*2帧之间位移变化量，单位cm*/
		opFlow.deltaPos[Y] = coeff * (opFlow.pixValid[Y] - opFlow.pixValidLast[Y]);	
		opFlow.pixValidLast[X] = opFlow.pixValid[X];	/*上一次实际输出像素*/
		opFlow.pixValidLast[Y] = opFlow.pixValid[Y];
		opFlow.deltaVel[X] = opFlow.deltaPos[X] / dt;	/*速度 cm/s*/
		opFlow.deltaVel[Y] = opFlow.deltaPos[Y] / dt;
		
#ifdef AVERAGE_FILTER
		velFilter(opFlow.deltaVel, opFlow.velLpf);		/*限幅均值滤波法*/
#else
		opFlow.velLpf[X] += (opFlow.deltaVel[X] - opFlow.velLpf[X]) * 0.15f;	/*速度低通 cm/s*/
		opFlow.velLpf[Y] += (opFlow.deltaVel[Y] - opFlow.velLpf[Y]) * 0.15f;	/*速度低通 cm/s*/
#endif			
		opFlow.velLpf[X] = constrainf(opFlow.velLpf[X], -VEL_LIMIT, VEL_LIMIT);	/*速度限幅 cm/s*/
		opFlow.velLpf[Y] = constrainf(opFlow.velLpf[Y], -VEL_LIMIT, VEL_LIMIT);	/*速度限幅 cm/s*/
	
		opFlow.posSum[X] += opFlow.deltaPos[X];	/*累积位移 cm*/
		opFlow.posSum[Y] += opFlow.deltaPos[Y];	/*累积位移 cm*/
	}
	else if(opFlow.isDataValid == true)
	{
		if(cnt++ > 100)	/*超过定点高度，切换为定高模式*/
		{
			cnt = 0;
			opFlow.isDataValid = false;
		}	
		resetOpFlowData();	
	}
	
	return opFlow.isOpFlowOk;	/*返回光流状态*/
}

/*光流电源控制
 *(九轴分支的 PMW3901 重构丢了本定义导致链接失败，合并时按主干实现补回)*/
void opticalFlowPowerControl(bool state)
{
	if(state == true)
		OPTICAL_POWER_ENABLE = true;
	else
		OPTICAL_POWER_ENABLE = false;
}

/*初始化光流模块*/
void opticalFlowInit(void)
{
	if (!isInit) /*第一次初始化通用IO*/
	{
		GPIO_InitTypeDef GPIO_InitStructure;

		//初始化CS引脚	
		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);//使能时钟
		
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
		GPIO_Init(GPIOB, &GPIO_InitStructure);		
	}
	else 
	{
		resetOpFlowData();
		opFlow.isOpFlowOk = true;				
	}
	
	opticalFlowPowerControl(true);	/*打开电源*/
	vTaskDelay(50);
	
	vTaskDelay(40);

	bool pmwReady = pmw3901Init();

	if (pmwReady)
	{
		if (isInit)
		{
			vTaskResume(opFlowTaskHandle);
		}
		else if(opFlowTaskHandle == NULL)
		{
			xTaskCreate(opticalFlowTask, "OPTICAL_FLOW", 300, NULL, 4, &opFlowTaskHandle);
		}
		isInit = true;
	}

	vl53lxxInit();
}

/*获取光流数据状态*/
bool getOpDataState(void)
{
	return opFlow.isDataValid;
}
	
	

