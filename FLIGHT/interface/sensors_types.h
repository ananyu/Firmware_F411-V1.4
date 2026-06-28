#ifndef __SENSORS_TYPES_H
#define __SENSORS_TYPES_H
#include "sys.h"
#include "axis.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * sensor参数类型定义	
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
********************************************************************************/

#if defined(__CC_ARM) 
	#pragma anon_unions
#endif

//union是一种特殊的数据类型，可以在同一内存位置存储不同的数据类型。通过使用union，可以节省内存空间，因为它允许不同的数据类型共享同一块内存区域。在这个代码中，定义了几个union类型，用于表示三轴的整数和浮点数数据。这些union类型可以方便地访问和操作三轴数据，同时节省内存空间。
//相当于同一份数据有两种访问方式，一种是通过结构体成员x、y、z访问，另一种是通过数组axis[0]、axis[1]、axis[2]访问。这种设计使得代码更加灵活，可以根据需要选择不同的访问方式。比如：A.x 等价于 A.axis[0]
typedef union 
{
	struct
	{
		int16_t x;
		int16_t y;
		int16_t z;
	};
	int16_t axis[3];
} Axis3i16;

typedef union 
{
	struct 
	{
		int32_t x;
		int32_t y;
		int32_t z;
	};
	int32_t axis[3];
} Axis3i32;

typedef union 
{
	struct 
	{
		int64_t x;
		int64_t y;
		int64_t z;
	};
	int64_t axis[3];
} Axis3i64;

typedef union 
{
	struct 
	{
		float x;
		float y;
		float z;
	};
	float axis[3];
} Axis3f;
 
#endif /* __SENSORS_TYPES_H */
