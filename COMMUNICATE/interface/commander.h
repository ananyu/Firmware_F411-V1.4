#ifndef __COMMANDER_H
#define __COMMANDER_H
#include "atkp.h"
#include "config.h"
#include "stabilizer_types.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * 获取遥控数据驱动代码
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
********************************************************************************/

#define COMMANDER_WDT_TIMEOUT_STABILIZE  500
#define COMMANDER_WDT_TIMEOUT_SHUTDOWN   1000

/*使用位段写法分配位置
存在问题：依赖编译器的
可升级方向为自己写位掩码*/
typedef struct
{
	u8 ctrlMode		: 2;	/*bit0  1=定高模式 0=手动模式   bit1  1=定点模式*/
	u8 keyFlight 	: 1;	/*bit2 一键起飞，这个标志位在整个飞行过程中存在，只有降落后才清除*/
	u8 keyLand 		: 1;	/*bit3 一键降落*/
	u8 emerStop 	: 1;	/*bit4 紧急停机*/
	u8 flightMode 	: 1;	/*bit5 飞行模式 1=无头 0=有头*/
	u8 reserved		: 2;	/*bit6~7 保留*/
}commanderBits_t;

/*控制数据结构体*/
typedef __packed struct
{
	float roll;       // 左右倾斜
	float pitch;      // 前后倾斜
	float yaw;        // 绕竖直轴转向
	float trimPitch;
	float trimRoll;
	u16 thrust;		//油门
} ctrlVal_t;

/*数据缓存结构体*/
typedef struct
{
	ctrlVal_t  tarVal[2];	//两份缓存
	bool activeSide;	//哪份是有效数据

	/*遥控数据新鲜，就优先用遥控
遥控没更新但 WiFi 还新鲜，就用 WiFi
两边都太久没更新，就进入保护，比如拉平、降落、锁定*/
	u32 timestamp; 		/* FreeRTOS 时钟节拍，记录收到时间，判断是否超时*/
} ctrlValCache_t;

typedef enum
{
	RATE    = 0,
	ANGLE   = 1,
} RPYType;

typedef enum
{
	XMODE     = 0, /*X模式*/
	CAREFREE  = 1, /*无头模式*/
} YawModeType;

typedef enum
{
	ATK_REMOTER = 0,
	WIFI		= 1,
}ctrlSrc_e;
	
void commanderInit(void);
bool commanderTest(void);
void flightCtrldataCache(ctrlSrc_e ctrlSrc, ctrlVal_t pk);
void commanderGetSetpoint(setpoint_t *setpoint, state_t *state);
void flyerAutoLand(setpoint_t *setpoint,const state_t *state);

void getAndUpdateTrim(float* pitch, float* roll);

void setCommanderCtrlMode(u8 set);
u8 getCommanderCtrlMode(void);

void setCommanderKeyFlight(bool set);
bool getCommanderKeyFlight(void);

void setCommanderKeyland(bool set);
bool getCommanderKeyland(void);

void setCommanderFlightmode(bool set);
void setCommanderEmerStop(bool set);

#endif /* __COMMANDER_H */
