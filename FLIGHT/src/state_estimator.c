#include "state_estimator.h"
#include "attitude_pid.h"
#include "position_pid.h"
#include "maths.h"
#include "vl53lxx.h"
#include "stabilizer.h"
#include "sensfusion6.h"
#include "optical_flow.h"

/********************************************************************************	 
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * 姿态估测代码	
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3 
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
 *
 * 修改说明:
 * 版本V1.3 位置估测代码移植于inav-1.9.0
********************************************************************************/

#define ACC_LIMIT			(1000.f)/*加速度限幅 单位cm/s/s*/
#define ACC_LIMIT_MAX		(1800.f)/*最大加速度限幅 单位cm/s/s*/
#define VELOCITY_LIMIT		(130.f)	/*速度限幅 单位cm/s*/
#define VELOCITY_LIMIT_MAX	(500.f)	/*最大速度限幅 单位cm/s*/

#define GRAVITY_CMSS 		(980.f)	/*重力加速度 单位cm/s/s*/
#define INAV_ACC_BIAS_ACCEPTANCE_VALUE	(GRAVITY_CMSS * 0.25f)   // Max accepted bias correction of 0.25G - unlikely we are going to be that much off anyway


static float wBaro = 0.35f;			/*气压校正权重*/
static float wOpflowP = 1.0f;		/*光流位置校正权重*/
static float wOpflowV = 2.0f;		/*光流速度校正权重*/
static float wAccBias = 0.01f;		/*加速度校正权重*/

static bool isRstHeight = false;	/*复位高度*/
static bool isRstAll = true;		/*复位估测*/

static float fusedHeight;			/*融合高度，起飞点为0*/
static float fusedHeightLpf = 0.f;	/*融合高度，低通*/
static float startBaroAsl = 0.f;	/*起飞点海拔*/



/*估测系统*/
static estimator_t estimator = 
{
	.vAccDeadband = 4.0f,
	.accBias[0] =  0.0f,
	.accBias[1] =  0.0f,
	.accBias[2] =  0.0f,
	.acc[0] = 0.0f,
	.acc[1] = 0.0f,
	.acc[2] = 0.0f,
	.vel[0] = 0.0f,
	.vel[1] = 0.0f,
	.vel[2] = 0.0f,
	.pos[0] = 0.0f,
	.pos[1] = 0.0f,
	.pos[2] = 0.0f,
};

/* Inertial filter, implementation taken from PX4 implementation by Anton Babushkin <rk3dov@gmail.com> */
static void inavFilterPredict(int axis, float dt, float acc)
{
	//s = vt + 1/2 at^2
	//v = v + at
    estimator.pos[axis] += estimator.vel[axis] * dt + acc * dt * dt / 2.0f;
    estimator.vel[axis] += acc * dt;
}
/*位置校正*/
static void inavFilterCorrectPos(int axis, float dt, float e, float w)
{
    float ewdt = e * w * dt;
    estimator.pos[axis] += ewdt;
    estimator.vel[axis] += w * ewdt;
}
/*速度校正*/
static void inavFilterCorrectVel(int axis, float dt, float e, float w)
{
   estimator.vel[axis] += e * w * dt;
}

void positionEstimate(sensorData_t* sensorData, state_t* state, float dt) 
{	
	bool hasFreshBaro = sensorData->baroUpdate;	/*气压计数据更新标志*/
	bool useZCorrection = false;	/*使用高度校正标志*/
	float errPosZ = 0.0f;

	static float rangeLpf = 0.f;
	static float accLpf[3] = {0.f};		/*加速度低通*/	
	float weight = wBaro;
	/*气压相对高度
	asl是软件计算的值，拿当前高度减去起飞时高度数据得到相对高度*/
	float relateHight = sensorData->baro.asl - startBaroAsl;	
	
	if(getModuleID()==OPTICAL_FLOW && isEnableVl53lxx==true)	/*光流模块可用,且使用激光*/
	{
		vl53lxxReadRange(&sensorData->zrange);	/*读取激光数据*/
	
//		rangeLpf = sensorData->zrange.distance;
		rangeLpf += (sensorData->zrange.distance - rangeLpf) * 0.1f;	/*低通 单位cm*/		
			
		float quality = sensorData->zrange.quality;		//可信度

		if(quality < 0.3f)	/*低于这个可行度，激光数据不可用*/
		{
			quality = 0.f;
		}

		if (quality > 0.f)
		{
			weight = quality;

			if (hasFreshBaro)
			{
				startBaroAsl = sensorData->baro.asl - rangeLpf;	/*起飞点海拔*/
			}

			fusedHeight = rangeLpf * quality + relateHight * (1.f - quality);	/*融合高度*/
			fusedHeightLpf += (fusedHeight - fusedHeightLpf) * 0.1f;	/*融合高度 低通*/
			useZCorrection = true;	/*使用高度校正*/
		}
		else if (hasFreshBaro)	/*激光数据不可用，使用气压计*/
		{
			fusedHeight = relateHight;	/*融合高度*/
			fusedHeightLpf += (fusedHeight - fusedHeightLpf) * 0.1f;	/*融合高度 低通*/
			useZCorrection = true;	/*使用高度校正*/
		}
	}
	else	/*无光流模块*/
	{
		if (hasFreshBaro)	/*气压计数据更新*/
		{
			fusedHeight = relateHight;	/*融合高度*/
			fusedHeightLpf += (fusedHeight - fusedHeightLpf) * 0.1f;	/*融合高度 低通*/
			useZCorrection = true;	/*使用高度校正*/
		}
		
	}
	
	
	if(isRstHeight)
	{	
		isRstHeight = false;
		
		weight = 0.95f;		/*增加权重，快速调整*/	
		
		startBaroAsl = sensorData->baro.asl;
		
		if(getModuleID() == OPTICAL_FLOW)
		{
			if(sensorData->zrange.distance < VL53L0X_MAX_RANGE)
			{
				startBaroAsl -= sensorData->zrange.distance;
				fusedHeight = sensorData->zrange.distance;
			}
		}
		
		estimator.pos[Z] = fusedHeight;
	}
	else if(isRstAll)
	{
		isRstAll = false;
		
		accLpf[Z] = 0.f;	
		fusedHeight  = 0.f;
		fusedHeightLpf = 0.f;
		startBaroAsl = sensorData->baro.asl;
		
		if(getModuleID() == OPTICAL_FLOW)
		{
			if(sensorData->zrange.distance < VL53L0X_MAX_RANGE)
			{
				startBaroAsl -= sensorData->zrange.distance;
				fusedHeight = sensorData->zrange.distance;		//立刻把高度修正到当前激光测距仪测量的高度，避免起飞时过高或者过低
			}
		}
		
		estimator.vel[Z] = 0.f;
		estimator.pos[Z] = fusedHeight;
	}
	
	
	Axis3f accelBF;
	/*取出加速度计三轴值
	把单位从 g 变成 cm/s^2
	减去估计出来的传感器偏置
	得到“机体系下的校正后加速度”*/
	accelBF.x = sensorData->acc.x * GRAVITY_CMSS - estimator.accBias[X];
	accelBF.y = sensorData->acc.y * GRAVITY_CMSS - estimator.accBias[Y];
	accelBF.z = sensorData->acc.z * GRAVITY_CMSS - estimator.accBias[Z];	
	
	/* Rotate vector to Earth frame - from Forward-Right-Down to North-East-Up*/
	imuTransformVectorBodyToEarth(&accelBF);	
	
	//死区的功能是让小的加速度读数被认为是0，避免因为传感器噪声导致的估测不稳定
	estimator.acc[X] = applyDeadbandf(accelBF.x, estimator.vAccDeadband);/*去除死区的加速度*/
	estimator.acc[Y] = applyDeadbandf(accelBF.y, estimator.vAccDeadband);/*去除死区的加速度*/
	estimator.acc[Z] = applyDeadbandf(accelBF.z, estimator.vAccDeadband);/*去除死区的加速度*/
	
	for(u8 i=0; i<3; i++)
		accLpf[i] += (estimator.acc[i] - accLpf[i]) * 0.1f;	/*加速度低通*/
		
	bool isKeyFlightLand = ((getCommanderKeyFlight()==true)||(getCommanderKeyland()==true));	/*定高飞或者降落状态*/
	
	if(isKeyFlightLand == true)		/*定高飞或者降落状态*/
	{
		state->acc.x = constrainf(accLpf[X], -ACC_LIMIT, ACC_LIMIT);	/*加速度限幅*/
		state->acc.y = constrainf(accLpf[Y], -ACC_LIMIT, ACC_LIMIT);	/*加速度限幅*/
		state->acc.z = constrainf(accLpf[Z], -ACC_LIMIT, ACC_LIMIT);	/*加速度限幅*/
	}else
	{
		state->acc.x = constrainf(estimator.acc[X], -ACC_LIMIT_MAX, ACC_LIMIT_MAX);	/*最大加速度限幅*/
		state->acc.y = constrainf(estimator.acc[Y], -ACC_LIMIT_MAX, ACC_LIMIT_MAX);	/*最大加速度限幅*/
		state->acc.z = constrainf(estimator.acc[Z], -ACC_LIMIT_MAX, ACC_LIMIT_MAX);	/*最大加速度限幅*/
	}		

	inavFilterPredict(Z, dt, estimator.acc[Z]);

	if (useZCorrection)
	{
		errPosZ = fusedHeight - estimator.pos[Z];
		inavFilterCorrectPos(Z, dt, errPosZ, weight);
	}
	

	if(getModuleID() == OPTICAL_FLOW)	/*光流模块可用*/
	{		
		float opflowDt = dt;
		
		float opResidualX = opFlow.posSum[X] - estimator.pos[X];
		float opResidualY = opFlow.posSum[Y] - estimator.pos[Y];
		float opResidualXVel = opFlow.velLpf[X] - estimator.vel[X];
		float opResidualYVel = opFlow.velLpf[Y] - estimator.vel[Y];
		
		float opWeightScaler = 1.0f;
		
		float wXYPos = wOpflowP * opWeightScaler;
		float wXYVel = wOpflowV * sq(opWeightScaler);
		
		/* 位置预估: XY-axis */
		inavFilterPredict(X, opflowDt, estimator.acc[X]);
		inavFilterPredict(Y, opflowDt, estimator.acc[Y]);
		/* 位置校正: XY-axis */
		inavFilterCorrectPos(X, opflowDt, opResidualX, wXYPos);
		inavFilterCorrectPos(Y, opflowDt, opResidualY, wXYPos);
		/* 速度校正: XY-axis */
		inavFilterCorrectVel(X, opflowDt, opResidualXVel, wXYVel);
		inavFilterCorrectVel(Y, opflowDt, opResidualYVel, wXYVel);
	}
	
	/*加速度偏置校正*/
	Axis3f accelBiasCorr = {{ 0, 0, 0}};
	
	if (useZCorrection)
	{
		accelBiasCorr.z -= errPosZ * sq(wBaro);
	}
	float accelBiasCorrMagnitudeSq = sq(accelBiasCorr.x) + sq(accelBiasCorr.y) + sq(accelBiasCorr.z);
	if (accelBiasCorrMagnitudeSq < sq(INAV_ACC_BIAS_ACCEPTANCE_VALUE)) 
	{
		/* transform error vector from NEU frame to body frame */
		imuTransformVectorEarthToBody(&accelBiasCorr);

		/* Correct accel bias */
		estimator.accBias[X] += accelBiasCorr.x * wAccBias * dt;
		estimator.accBias[Y] += accelBiasCorr.y * wAccBias * dt;
		estimator.accBias[Z] += accelBiasCorr.z * wAccBias * dt;
	}	

	if(isKeyFlightLand == true)		/*定高飞或者降落状态*/
	{
		state->velocity.x = constrainf(estimator.vel[X], -VELOCITY_LIMIT, VELOCITY_LIMIT);	/*速度限幅 VELOCITY_LIMIT*/
		state->velocity.y = constrainf(estimator.vel[Y], -VELOCITY_LIMIT, VELOCITY_LIMIT);	/*速度限幅 VELOCITY_LIMIT*/
		state->velocity.z = constrainf(estimator.vel[Z], -VELOCITY_LIMIT, VELOCITY_LIMIT);	/*速度限幅 VELOCITY_LIMIT*/
	}else
	{
		state->velocity.x = constrainf(estimator.vel[X], -VELOCITY_LIMIT_MAX, VELOCITY_LIMIT_MAX);	/*最大速度限幅 VELOCITY_LIMIT_MAX*/
		state->velocity.y = constrainf(estimator.vel[Y], -VELOCITY_LIMIT_MAX, VELOCITY_LIMIT_MAX);	/*最大速度限幅 VELOCITY_LIMIT_MAX*/
		state->velocity.z = constrainf(estimator.vel[Z], -VELOCITY_LIMIT_MAX, VELOCITY_LIMIT_MAX);	/*最大速度限幅 VELOCITY_LIMIT_MAX*/
	}
	
	state->position.x = estimator.pos[X];
	state->position.y = estimator.pos[Y];
	state->position.z = estimator.pos[Z];	
}

/*读取融合高度 单位cm*/	
float getFusedHeight(void)
{
	return fusedHeightLpf;
}

/*复位估测高度*/
void estRstHeight(void)
{
	isRstHeight = true;
}

/*复位所有估测*/
void estRstAll(void)
{
	isRstAll = true;
}


