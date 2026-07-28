#include <math.h>
#include "stdio.h"
#include "delay.h"
#include "config.h"
#include "config_param.h"
#include "mag_calibration.h"
#include "ledseq.h"
#include "mpu9250.h"
#include "sensors.h"
#include "ak8963.h"
#include "bmp280.h"
#include "filter.h"
#include "axis.h"
#include "spl06.h"


/*FreeRTOS相关头文件*/
#include "FreeRTOS.h"
#include "task.h"

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
//FS为设置量程，LSB为转化单位，将传感器数值转为实际的物理量
#define SENSORS_GYRO_FS_CFG       MPU9250_GYRO_FS_2000
#define SENSORS_DEG_PER_LSB_CFG   MPU9250_DEG_PER_LSB_2000

#define SENSORS_ACCEL_FS_CFG      MPU9250_ACCEL_FS_16
#define SENSORS_G_PER_LSB_CFG     MPU9250_G_PER_LSB_16

#define SENSORS_NBR_OF_BIAS_SAMPLES		1024	/* 计算方差的采样样本个数 */
#define GYRO_VARIANCE_BASE				4000	/* 陀螺仪零偏方差阈值 */
#define SENSORS_ACC_SCALE_SAMPLES  		200		/* 加速计采样个数 */

// MPU9250主机模式读取数据 缓冲区长度
#define SENSORS_MPU9250_BUFF_LEN    14		/*ACC+GYRO+TEMP*/
#define SENSORS_MAG_BUFF_LEN       	8		/*MAG, ST1+XYZ+ST2*/
#define SENSORS_BARO_STATUS_LEN		1		/*气压计状态寄存器长度*/
#define SENSORS_BARO_DATA_LEN		6		/*气压计数据寄存器长度,压力3+温度3*/
#define SENSORS_BARO_BUFF_LEN       (SENSORS_BARO_STATUS_LEN + SENSORS_BARO_DATA_LEN)

typedef struct
{
	Axis3f     bias;
	bool       isBiasValueFound;
	bool       isBufferFilled;
	Axis3i16*  bufHead;
	Axis3i16   buffer[SENSORS_NBR_OF_BIAS_SAMPLES];
}BiasObj;

typedef struct
{
	Axis3f acc;
	Axis3f gyro;
	float temperature;
} fast_sensor_data_t;

typedef struct
{
	Axis3f mag;
	baro_t baro;
} slow_sensor_data_t;

static fast_sensor_data_t fastSensorData;
static slow_sensor_data_t slowSensorData;

BiasObj	gyroBiasRunning;
static Axis3f  gyroBias;

static bool gyroBiasFound = false;
static float accScaleSum = 0;
static float accScale = 1;

static bool isInit = false;
//static sensorData_t sensors;
static Axis3i16	gyroRaw;
static Axis3i16	accRaw;
static Axis3i16 magRaw;

/*低通滤波参数*/
#define GYRO_LPF_CUTOFF_FREQ  80
#define ACCEL_LPF_CUTOFF_FREQ 30
static lpf2pData accLpf[3];
static lpf2pData gyroLpf[3];

static bool isMPUPresent=false;
static bool isMagPresent=false;
static bool isBaroPresent=false;
static bool magCalibrationActive = false;
static u16 magCalibrationSamples = 0;
static float magCalibrationMin[3];
static float magCalibrationMax[3];
static float magFactoryAdjustment[3] = {1.0f, 1.0f, 1.0f};

enum {IDLE, BMP280, SPL06}baroType = IDLE;

//static uint8_t buffer[SENSORS_MPU9250_BUFF_LEN + SENSORS_MAG_BUFF_LEN + SENSORS_BARO_BUFF_LEN] = {0};

static uint8_t imuBuffer[SENSORS_MPU9250_BUFF_LEN];
static uint8_t magBuffer[SENSORS_MAG_BUFF_LEN];
// static uint8_t baroBuffer[SENSORS_BARO_DATA_LEN];
static uint8_t baroBuffer[SENSORS_BARO_BUFF_LEN];

static xQueueHandle accelerometerDataQueue;
static xQueueHandle gyroDataQueue;
static xQueueHandle magnetometerDataQueue;
static xQueueHandle barometerDataQueue;
static xSemaphoreHandle sensorsDataReady;

static void applyAxis3fLpf(lpf2pData *data, Axis3f* in);
static void sensorsBiasObjInit(BiasObj* bias);
static void sensorsCalculateVarianceAndMean(BiasObj* bias, Axis3f* varOut, Axis3f* meanOut);
static bool sensorsFindBiasValue(BiasObj* bias);
static void sensorsAddBiasValue(BiasObj* bias, int16_t x, int16_t y, int16_t z);
static void sensorsTaskFast(void *param);
static void sensorsTaskSlow(void *param);




/*从队列读取陀螺数据*/
bool sensorsReadGyro(Axis3f *gyro)
{
	//语法（句柄，数据指针，等待时间）从队列读取数据，如果队列里没有数据了，就返回false
	return (pdTRUE == xQueueReceive(gyroDataQueue, gyro, 0));
}
/*从队列读取加速计数据*/
bool sensorsReadAcc(Axis3f *acc)
{
	return (pdTRUE == xQueueReceive(accelerometerDataQueue, acc, 0));
}
/*从队列读取磁力计数据*/
bool sensorsReadMag(magSample_t *sample)
{
	return (pdTRUE == xQueueReceive(magnetometerDataQueue, sample, 0));
}
/*从队列读取气压数据*/
bool sensorsReadBaro(baro_t *baro)
{
	return (pdTRUE == xQueueReceive(barometerDataQueue, baro, 0));
}
/*传感器中断初始化*/
static void sensorsInterruptInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	EXTI_InitTypeDef EXTI_InitStructure;

	/*使能MPU9250中断*/
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;	//内部下拉
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	//先把对应GPIOA的引脚连接上中断控制器
	SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOA, EXTI_PinSource4);

	EXTI_InitStructure.EXTI_Line = EXTI_Line4;
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	portDISABLE_INTERRUPTS();	//先屏蔽中断，再设置进去，之后再打开
	EXTI_Init(&EXTI_InitStructure);
	EXTI_ClearITPendingBit(EXTI_Line4);
	portENABLE_INTERRUPTS();
}

/* 传感器器件初始化 */
void sensorsDeviceInit(void)
{
	i2cdevInit(SENSORS_I2CBUS);
	mpu9250Init(SENSORS_I2CBUS, MPU9250_DEFAULT_ADDRESS);
	
	vTaskDelay(10);
	mpu9250Reset();	// 复位MPU9250
	vTaskDelay(20);	// 延时等待寄存器复位
	
	u8 temp = mpu9250GetDeviceID();
	if (temp == MPU9250_WHO_AM_I_VALUE)
	{
		isMPUPresent=true;
		printf("MPU9250 I2C connection [OK].\n");
	}
	else
	{
		printf("MPU9250 I2C connection [FAIL].\n");
	}
	
	mpu9250SetSleepEnabled(false);	// 唤醒MPU9250
	vTaskDelay(10);		
	mpu9250SetClockSource(MPU9250_CLOCK_PLL_XGYRO);	// 设置X轴陀螺作为时钟，提高稳定性
	vTaskDelay(10);		// 延时等待时钟稳定	
	mpu9250SetTempSensorEnabled(true);	// 使能温度传感器	
	mpu9250SetIntEnabled(false);		// 关闭中断	
	mpu9250SetI2CBypassEnabled(true);	// 旁路模式，磁力计和气压连接到主IIC	
	mpu9250SetFullScaleGyroRange(SENSORS_GYRO_FS_CFG);	// 设置陀螺量程	
	mpu9250SetFullScaleAccelRange(SENSORS_ACCEL_FS_CFG);// 设置加速计量程	
	
	mpu9250SetAccelFchoiceb(MPU9250_ACCEL_FCHOICE_B_DLPF); // 选择加速度计 DLPF 路径
	mpu9250SetAccelDLPF(MPU9250_ACCEL_DLPF_CFG_3);         // A_DLPF_CFG=3，约 44.8Hz
	mpu9250SetGyroFchoiceb(MPU9250_GYRO_FCHOICE_B_DLPF);   // 选择陀螺仪 DLPF 路径
	mpu9250SetDLPFMode(MPU9250_DLPF_BW_92);                // DLPF_CFG=2，陀螺仪约 92Hz
	mpu9250SetRate(0);// 设置采样速率: 1000 / (1 + 0) = 1000Hz

	for (u8 i = 0; i < 3; i++)// 初始化加速计和陀螺二阶低通滤波
	{
		lpf2pInit(&gyroLpf[i], 1000, GYRO_LPF_CUTOFF_FREQ);
		lpf2pInit(&accLpf[i],  1000, ACCEL_LPF_CUTOFF_FREQ);
	}


#ifdef SENSORS_ENABLE_MAG_AK8963
	ak8963Init(SENSORS_I2CBUS, AK8963_DEFAULT_ADDRESS);	//ak8963磁力计初始化
	if (ak8963TestConnection() == true)
	{
		if (ak8963ReadFactoryAdjustment(magFactoryAdjustment))
		{
			isMagPresent = true;
			ak8963SetMode(AK8963_MODE_16BIT | AK8963_MODE_CONT2); // 16bit 100Hz
			printf("AK8963 I2C connection [OK].\n");
		}
		else
		{
			printf("AK8963 factory adjustment [FAIL].\n");
		}
	}
	else
	{
		printf("AK8963 I2C connection [FAIL].\n");
	}
#endif

	if (bmp280Init(SENSORS_I2CBUS, BMP280_I2C_ADDR) == true)//BMP280初始化
	{
		isBaroPresent = true;
		baroType = BMP280;
		vTaskDelay(100);
	}
	else if (SPL06Init(SENSORS_I2CBUS) == true)//SPL06初始化
	{
		isBaroPresent = true;
		baroType = SPL06;
		vTaskDelay(100);
	}
	else
	{
		isBaroPresent = false;
	}

	/*创建传感器数据队列*/
	accelerometerDataQueue = xQueueCreate(1, sizeof(Axis3f));
	gyroDataQueue = xQueueCreate(1, sizeof(Axis3f));
	magnetometerDataQueue = xQueueCreate(1, sizeof(magSample_t));
	barometerDataQueue = xQueueCreate(1, sizeof(baro_t));
}
/*传感器偏置初始化*/
static void sensorsBiasObjInit(BiasObj* bias)
{
	bias->isBufferFilled = false;
	bias->bufHead = bias->buffer;
}
/*传感器测试*/
bool sensorsTest(void)
{
	bool testStatus = true;

	if (!isInit)
	{
		printf("Uninitialized\n");
		testStatus = false;
	}

	testStatus&=isBaroPresent;
	
	return testStatus;
}

/*计算方差和平均值*/
static void sensorsCalculateVarianceAndMean(BiasObj* bias, Axis3f* varOut, Axis3f* meanOut)
{
	u32 i;
	int64_t sum[3] = {0};
	int64_t sumsq[3] = {0};

	for (i = 0; i < SENSORS_NBR_OF_BIAS_SAMPLES; i++)
	{
		sum[0] += bias->buffer[i].x;
		sum[1] += bias->buffer[i].y;
		sum[2] += bias->buffer[i].z;
		sumsq[0] += bias->buffer[i].x * bias->buffer[i].x;
		sumsq[1] += bias->buffer[i].y * bias->buffer[i].y;
		sumsq[2] += bias->buffer[i].z * bias->buffer[i].z;
	}

	//一种方差的等效表达，
	varOut->x = (sumsq[0] - ((int64_t)sum[0] * sum[0]) / SENSORS_NBR_OF_BIAS_SAMPLES);
	varOut->y = (sumsq[1] - ((int64_t)sum[1] * sum[1]) / SENSORS_NBR_OF_BIAS_SAMPLES);
	varOut->z = (sumsq[2] - ((int64_t)sum[2] * sum[2]) / SENSORS_NBR_OF_BIAS_SAMPLES);

	meanOut->x = (float)sum[0] / SENSORS_NBR_OF_BIAS_SAMPLES;
	meanOut->y = (float)sum[1] / SENSORS_NBR_OF_BIAS_SAMPLES;
	meanOut->z = (float)sum[2] / SENSORS_NBR_OF_BIAS_SAMPLES;
}
/*传感器查找偏置值*/
static bool sensorsFindBiasValue(BiasObj* bias)
{
	bool foundbias = false;

	if (bias->isBufferFilled)
	{
		
		Axis3f mean;
		Axis3f variance;
		sensorsCalculateVarianceAndMean(bias, &variance, &mean);

		if (variance.x < GYRO_VARIANCE_BASE && variance.y < GYRO_VARIANCE_BASE && variance.z < GYRO_VARIANCE_BASE)
		{
			//如果方差小于阈值，认为找到了偏置值，保存平均值到bias结构体中，并设置标志位
			bias->bias.x = mean.x;
			bias->bias.y = mean.y;
			bias->bias.z = mean.z;
			foundbias = true;
			bias->isBiasValueFound= true;
		}else
			bias->isBufferFilled=false;
	}
	return foundbias;
}

static void sensorSetupImuDataReadyInterrupt(void)
{
	mpu9250SetInterruptMode(0); 		// 中断高电平有效
	mpu9250SetInterruptDrive(0); 		// 推挽输出
	mpu9250SetInterruptLatch(0); 		// 中断锁存模式(0=50us-pulse, 1=latch-until-int-cleared)
	mpu9250SetInterruptLatchClear(1); 	// 中断清除模式(0=status-read-only, 1=any-register-read)
	
	mpu9250SetWaitForExternalSensorEnabled(false); 	// 等待外部传感器数据就绪
	mpu9250SetIntDataReadyEnabled(true);	// 使能数据就绪中断
	mpu9250SetIntEnabled(true);		// 使能中断
}

/* 传感器初始化 */
void sensorsInit(void)
{
	if(isInit) return;

	sensorsDataReady = xSemaphoreCreateBinary();/*创建传感器数据就绪二值信号量*/
	sensorsBiasObjInit(&gyroBiasRunning);	//将陀螺仪环形缓冲区的指针指向缓冲区的开始，并把标志位清零
	sensorsDeviceInit();	/*传感器器件初始化*/
	sensorsInterruptInit();	/*传感器中断初始化*/
	
	isInit = true;
}


/*设置传感器从模式读取，根据PX4方案快慢方案这个需要关闭*/
static void sensorsSetupSlaveRead(void)
{
	mpu9250SetSlave4MasterDelay(4); 	// 从机读取速率: 100Hz = (1000Hz / (1 + 9))

	mpu9250SetI2CBypassEnabled(false);	//主机模式
	mpu9250SetWaitForExternalSensorEnabled(true); 	
	mpu9250SetInterruptMode(0); 		// 中断高电平有效
	mpu9250SetInterruptDrive(0); 		// 推挽输出
	mpu9250SetInterruptLatch(0); 		// 中断锁存模式(0=50us-pulse, 1=latch-until-int-cleared)
	mpu9250SetInterruptLatchClear(1); 	// 中断清除模式(0=status-read-only, 1=any-register-read)
	mpu9250SetSlaveReadWriteTransitionEnabled(false); // 关闭从机读写传输
	mpu9250SetMasterClockSpeed(I2C_MST_CLK_400_kHz); 	// 设置i2c速度400kHz

#ifdef SENSORS_ENABLE_MAG_AK8963
	if (isMagPresent)
	{
		// 设置MPU9250主机要读取的寄存器
		mpu9250SetSlaveAddress(0, MPU9250_I2C_SLV_ADDR_READ | AK8963_ADDRESS_00); 	// 设置磁力计为0号从机
		mpu9250SetSlaveRegister(0, AK8963_RA_ST1); 				// 从机0需要读取的寄存器
		mpu9250SetSlaveDataLength(0, SENSORS_MAG_BUFF_LEN); 	// 读取8个字节(ST1, x, y, z heading, ST2 (overflow check))
		mpu9250SetSlaveDelayEnabled(0, true);
		mpu9250SetSlaveEnabled(0, true);
	}
#endif


	if (isBaroPresent && baroType == BMP280)
	{
		// 设置MPU9250主机要读取BMP280的寄存器
		mpu9250SetSlaveAddress(1, MPU9250_I2C_SLV_ADDR_READ | BMP280_I2C_ADDR);		// 设置气压计状态寄存器为1号从机
		mpu9250SetSlaveRegister(1, BMP280_STAT_REG);			// 从机1需要读取的寄存器
		mpu9250SetSlaveDataLength(1, SENSORS_BARO_STATUS_LEN);	// 读取1个字节
		mpu9250SetSlaveDelayEnabled(1, true);
		mpu9250SetSlaveEnabled(1, true);

		mpu9250SetSlaveAddress(2, MPU9250_I2C_SLV_ADDR_READ | BMP280_I2C_ADDR);		// 设置气压计数据寄存器为2号从机
		mpu9250SetSlaveRegister(2, BMP280_PRESSURE_MSB_REG);	// 从机2需要读取的寄存器
		mpu9250SetSlaveDataLength(2, SENSORS_BARO_DATA_LEN);	// 读取6个字节
		mpu9250SetSlaveDelayEnabled(2, true);
		mpu9250SetSlaveEnabled(2, true);
	}
	if (isBaroPresent && baroType == SPL06)
	{
		// 设置MPU9250主机要读取SPL06的寄存器
		mpu9250SetSlaveAddress(1, MPU9250_I2C_SLV_ADDR_READ | SPL06_I2C_ADDR);		// 设置气压计状态寄存器为1号从机
		mpu9250SetSlaveRegister(1, SPL06_MODE_CFG_REG);			// 从机1需要读取的寄存器
		mpu9250SetSlaveDataLength(1, SENSORS_BARO_STATUS_LEN);	// 读取1个字节
		mpu9250SetSlaveDelayEnabled(1, true);
		mpu9250SetSlaveEnabled(1, true);

		mpu9250SetSlaveAddress(2, MPU9250_I2C_SLV_ADDR_READ | SPL06_I2C_ADDR);		// 设置气压计数据寄存器为2号从机
		mpu9250SetSlaveRegister(2, SPL06_PRESSURE_MSB_REG);		// 从机2需要读取的寄存器
		mpu9250SetSlaveDataLength(2, SENSORS_BARO_DATA_LEN);	// 读取6个字节
		mpu9250SetSlaveDelayEnabled(2, true);
		mpu9250SetSlaveEnabled(2, true);
	}

	mpu9250SetI2CMasterModeEnabled(true);	//使能mpu9250主机模式
	mpu9250SetIntDataReadyEnabled(true);	//数据就绪中断使能
}

/**
 * 往方差缓冲区（循环缓冲区）添加一个新值，缓冲区满后，替换旧的的值
 */
static void sensorsAddBiasValue(BiasObj* bias, int16_t x, int16_t y, int16_t z)
{
	bias->bufHead->x = x;
	bias->bufHead->y = y;
	bias->bufHead->z = z;
	bias->bufHead++;

	//在指针中，可以使用数组最后一个进行比较，但是没法解引用和操作，只是用来表示尾部
	if (bias->bufHead >= &bias->buffer[SENSORS_NBR_OF_BIAS_SAMPLES])
	{
		bias->bufHead = bias->buffer;
		bias->isBufferFilled = true;
	}
}

/**
 * 根据样本计算重力加速度缩放因子
 */
static bool processAccScale(int16_t ax, int16_t ay, int16_t az)
{
	static bool accBiasFound = false;
	static uint32_t accScaleSumCount = 0;

	if (!accBiasFound)
	{
		//换算，ax还是读取的原始传感器量化值，需要乘系数转化为浮点数，然后计算加速度的模长，累加到accScaleSum中，最后除以样本个数得到平均值，作为加速度的缩放因子
		accScaleSum += sqrtf(powf(ax * SENSORS_G_PER_LSB_CFG, 2) + powf(ay * SENSORS_G_PER_LSB_CFG, 2) + powf(az * SENSORS_G_PER_LSB_CFG, 2));
		accScaleSumCount++;

		if (accScaleSumCount == SENSORS_ACC_SCALE_SAMPLES)
		{
			accScale = accScaleSum / SENSORS_ACC_SCALE_SAMPLES;
			accBiasFound = true;
		}
	}

	return accBiasFound;
}

/**
 * 计算陀螺方差
 */
static bool processGyroBias(int16_t gx, int16_t gy, int16_t gz, Axis3f *gyroBiasOut)
{
	sensorsAddBiasValue(&gyroBiasRunning, gx, gy, gz);

	if (!gyroBiasRunning.isBiasValueFound)
	{
		sensorsFindBiasValue(&gyroBiasRunning);
	}

	gyroBiasOut->x = gyroBiasRunning.bias.x;
	gyroBiasOut->y = gyroBiasRunning.bias.y;
	gyroBiasOut->z = gyroBiasRunning.bias.z;

	return gyroBiasRunning.isBiasValueFound;
}

/*处理气压计数据*/
void processBarometerMeasurements(const u8 *buffer)
{
	static float temp;
    static float pressure;
	
	if (baroType == BMP280)
	{
		// Check if there is a new data update
		//if ((buffer[0] & 0x08)) /*bit3=1 转换完成*/
		//{
			s32 rawPressure = (s32)((((u32)(buffer[1])) << 12) | (((u32)(buffer[2])) << 4) | ((u32)buffer[3] >> 4));
			s32 rawTemp = (s32)((((u32)(buffer[4])) << 12) | (((u32)(buffer[5])) << 4) | ((u32)buffer[6] >> 4));
			temp = bmp280CompensateT(rawTemp)/100.0f;		
			pressure = bmp280CompensateP(rawPressure)/25600.0f;			

			//pressureFilter(&pressure, &slowSensorData.baro.pressure);	
			slowSensorData.baro.pressure = pressure;
			slowSensorData.baro.temperature = (float)temp;	/*单位度*/
			slowSensorData.baro.asl = bmp280PressureToAltitude(&pressure) * 100.f;	/*转换成海拔*/
		//}
	}
	else if (baroType == SPL06)
	{
		s32 rawPressure = (int32_t)buffer[1]<<16 | (int32_t)buffer[2]<<8 | (int32_t)buffer[3];
		rawPressure = (rawPressure & 0x800000) ? (0xFF000000 | rawPressure) : rawPressure;
		
		s32 rawTemp = (int32_t)buffer[4]<<16 | (int32_t)buffer[5]<<8 | (int32_t)buffer[6];
		rawTemp = (rawTemp & 0x800000) ? (0xFF000000 | rawTemp) : rawTemp;
		
		temp = spl0601_get_temperature(rawTemp);
		pressure = spl0601_get_pressure(rawPressure, rawTemp);
		slowSensorData.baro.pressure = pressure / 100.0f;
		slowSensorData.baro.temperature = (float)temp; /*单位度*/
		slowSensorData.baro.asl = SPL06PressureToAltitude(slowSensorData.baro.pressure) * 100.f; //cm
	}

}
/*处理磁力计数据*/
void sensorsMagCalibrationStart(void)
{
	for (u8 axis = 0; axis < 3; axis++)
	{
		magCalibrationMin[axis] = 3.4e38f;
		magCalibrationMax[axis] = -3.4e38f;
	}
	magCalibrationSamples = 0;
	magCalibrationActive = true;
}

void sensorsMagCalibrationCancel(void)
{
	magCalibrationActive = false;
	magCalibrationSamples = 0;
}

bool sensorsMagCalibrationFinish(void)
{
	magCalibrationResult_t result;
	float minimum[3];
	float maximum[3];
	float fieldMagnitude = 0.0f;

	if (!magCalibrationActive || magCalibrationSamples < 200U)
	{
		return false;
	}
	for (u8 axis = 0; axis < 3; axis++)
	{
		minimum[axis] = magCalibrationMin[axis];
		maximum[axis] = magCalibrationMax[axis];
	}
	if (!magCalibrationCompute(minimum, maximum, &result))
	{
		return false;
	}
	for (u8 axis = 0; axis < 3; axis++)
	{
		if (result.scale[axis] < 0.5f || result.scale[axis] > 2.0f) return false;
		configParam.magCalibration.offset[axis] = result.offset[axis];
		configParam.magCalibration.scale[axis] = result.scale[axis];
		fieldMagnitude += 0.5f * (maximum[axis] - minimum[axis]) * result.scale[axis];
	}
	configParam.magCalibration.fieldMagnitude = fieldMagnitude / 3.0f;
	configParam.magCalibration.valid = true;
	magCalibrationActive = false;
	saveConfigAndNotify();
	return true;
}
static Axis3f sensorsMapMagToBody(const Axis3f raw)
{
	Axis3f body;
	body.x = raw.y;
	body.y = -raw.x;
	body.z = raw.z;
	return body;
}

static bool sensorsProcessMagSample(const uint8_t *buffer, magSample_t *sample)
{
	Axis3f raw;
	Axis3f field;
	float values[3];
	magCalibrationResult_t calibration;

	if (!(buffer[0] & (1U << AK8963_ST1_DRDY_BIT)) ||
		(buffer[7] & (1U << AK8963_ST2_DERR_BIT)) ||
		(buffer[7] & (1U << AK8963_ST2_HOFL_BIT)))
	{
		return false;
	}

	magRaw.x = (int16_t)(((int16_t)buffer[2] << 8) | buffer[1]);
	magRaw.y = (int16_t)(((int16_t)buffer[4] << 8) | buffer[3]);
	magRaw.z = (int16_t)(((int16_t)buffer[6] << 8) | buffer[5]);
	raw.x = ((float)magRaw.x * magFactoryAdjustment[0]) / MAG_GAUSS_PER_LSB;
	raw.y = ((float)magRaw.y * magFactoryAdjustment[1]) / MAG_GAUSS_PER_LSB;
	raw.z = ((float)magRaw.z * magFactoryAdjustment[2]) / MAG_GAUSS_PER_LSB;
	field = sensorsMapMagToBody(raw);

	if (magCalibrationActive)
	{
		for (u8 axis = 0; axis < 3; axis++)
		{
			if (field.axis[axis] < magCalibrationMin[axis]) magCalibrationMin[axis] = field.axis[axis];
			if (field.axis[axis] > magCalibrationMax[axis]) magCalibrationMax[axis] = field.axis[axis];
		}
		magCalibrationSamples++;
	}
	if (configParam.magCalibration.valid)
	{
		for (u8 axis = 0; axis < 3; axis++)
		{
			calibration.offset[axis] = configParam.magCalibration.offset[axis];
			calibration.scale[axis] = configParam.magCalibration.scale[axis];
			values[axis] = field.axis[axis];
		}
		magCalibrationApply(&calibration, values, values);
		for (u8 axis = 0; axis < 3; axis++)
		{
			field.axis[axis] = values[axis];
		}
	}

	sample->field = field;
	sample->magnitude = sqrtf(field.x * field.x + field.y * field.y + field.z * field.z);
	sample->timestamp = (u32)xTaskGetTickCount();
	sample->valid = sample->magnitude > 0.0f;
	return sample->valid;
}
void processMagnetometerMeasurements(const uint8_t *buffer)
{
	if (buffer[0] & (1 << AK8963_ST1_DRDY_BIT)) 
	{
		int16_t headingx = (((int16_t) buffer[2]) << 8) | buffer[1];
		int16_t headingy = (((int16_t) buffer[4]) << 8) | buffer[3];
		int16_t headingz = (((int16_t) buffer[6]) << 8) | buffer[5];

		slowSensorData.mag.x = (float)headingx / MAG_GAUSS_PER_LSB;
		slowSensorData.mag.y = (float)headingy / MAG_GAUSS_PER_LSB;
		slowSensorData.mag.z = (float)headingz / MAG_GAUSS_PER_LSB;		
		magRaw.x = headingx;/*用于上传到上位机*/
		
		magRaw.y = headingy;
		magRaw.z = headingz;
	}
}
/*处理加速计和陀螺仪数据，buffer里面还是读的原始数据，需要转化一下*/
void processAccGyroMeasurements(const uint8_t *buffer)
{
	/*注意传感器读取方向(旋转270°x和y交换)*/
	int16_t ay = (((int16_t) buffer[0]) << 8) | buffer[1];
	int16_t ax = ((((int16_t) buffer[2]) << 8) | buffer[3]);
	int16_t az = (((int16_t) buffer[4]) << 8) | buffer[5];
	int16_t gy = (((int16_t) buffer[8]) << 8) | buffer[9];
	int16_t gx = (((int16_t) buffer[10]) << 8) | buffer[11];
	int16_t gz = (((int16_t) buffer[12]) << 8) | buffer[13];

	accRaw.x = ax;/*用于上传到上位机*/
	accRaw.y = ay;
	accRaw.z = az;
	//陀螺仪校准偏差，后面每次读取都需要 减去这个值
	gyroRaw.x = gx - gyroBias.x;
	gyroRaw.y = gy - gyroBias.y;
	gyroRaw.z = gz - gyroBias.z;
//设置已经校准完成标志，计算陀螺零偏和加速计缩放因子
	gyroBiasFound = processGyroBias(gx, gy, gz, &gyroBias);
	//ACC校准需要等陀螺仪ok后才行，所以可能可以优化一下
	if (gyroBiasFound)
	{
		processAccScale(ax, ay, az);	/*计算accScale*/
	}
	
	//将原始数据转化为物理量，陀螺仪需要减去偏置值，乘以转化系数，得到单位为°/s的角速度值；加速计直接乘以转化系数和缩放因子，得到单位为g的加速度值
	fastSensorData.gyro.x = -(gx - gyroBias.x) * SENSORS_DEG_PER_LSB_CFG;	/*单位 °/s */
	fastSensorData.gyro.y =  (gy - gyroBias.y) * SENSORS_DEG_PER_LSB_CFG;
	fastSensorData.gyro.z =  (gz - gyroBias.z) * SENSORS_DEG_PER_LSB_CFG;
	applyAxis3fLpf(gyroLpf, &fastSensorData.gyro);	

	fastSensorData.acc.x = -(ax) * SENSORS_G_PER_LSB_CFG / accScale;	/*单位 g(9.8m/s^2)*/
	fastSensorData.acc.y =  (ay) * SENSORS_G_PER_LSB_CFG / accScale;	/*重力加速度缩放因子accScale 根据样本计算得出*/
	fastSensorData.acc.z =  (az) * SENSORS_G_PER_LSB_CFG / accScale;

	applyAxis3fLpf(accLpf, &fastSensorData.acc);
}
/*传感器任务*/
void sensorsTask(void *param)
{
	(void)param;

	sensorsInit();	/*传感器初始化*/
	vTaskDelay(pdMS_TO_TICKS(1000));	/*延时1s，等待传感器稳定*/
	//sensorsSetupSlaveRead();/*设置传感器从模式读取，关闭，改为快慢读取方案*/
	sensorSetupImuDataReadyInterrupt();	/*设置MPU9250中断，数据就绪中断*/

	vTaskSuspendAll();	
	xTaskCreate(sensorsTaskFast, "sensorsTaskFast", 350, NULL, 6, NULL);
	xTaskCreate(sensorsTaskSlow, "sensorsTaskSlow", 300, NULL, 3, NULL);
	xTaskResumeAll();

	vTaskDelete(NULL);
}

static void sensorsTaskFast(void *param)
{
	(void)param;

	while (1)
	{
		//阻塞等待，等数据准备好就开始拿，xSemaphoreTake会把信号量减1，如果没有信号量了，
		// 就会一直等下去，直到有任务释放这个信号量，或者等到超时（这里是无限等待）
		if (pdTRUE == xSemaphoreTake(sensorsDataReady, portMAX_DELAY))
		{
			/*确定数据长度*/
			u8 dataLen = (u8) (SENSORS_MPU9250_BUFF_LEN);

			//传入参数（通过 I2C1 这条总线，从 MPU9250 这个器件的某个起始寄存器开始，连续读取 dataLen 个字节，存到 buffer 里。
			i2cdevRead(I2C1_DEV, MPU9250_ADDRESS_AD0_HIGH, MPU9250_RA_ACCEL_XOUT_H, dataLen, imuBuffer);
			
			/*处理原始数据，带了滤波，校准等*/
			processAccGyroMeasurements(&(imuBuffer[0]));
			
			//挂起任务，防止其他任务切换导致队列没完全写完
			vTaskSuspendAll();	/*确保同一时刻把数据放入队列中，因为后面有其他任务使用数据，避免拿到一帧新一帧旧*/
			xQueueOverwrite(accelerometerDataQueue, &fastSensorData.acc);
			xQueueOverwrite(gyroDataQueue, &fastSensorData.gyro);
			
			xTaskResumeAll();
		}
	}	
}

static void sensorsTaskSlow(void *param)
{
	(void)param;
	TickType_t lastWakeTime = xTaskGetTickCount();
	uint8_t slowTick = 0;

	while (1)
	{
		//vTaskDelay(10);	//延时10ms，100Hz
		if (isMagPresent)
		{
			magSample_t sample;
			i2cdevRead(I2C1_DEV, AK8963_ADDRESS_00, AK8963_RA_ST1, SENSORS_MAG_BUFF_LEN, magBuffer);
			if (sensorsProcessMagSample(&(magBuffer[0]), &sample))
			{
				xQueueOverwrite(magnetometerDataQueue, &sample);
			}
		}
		
		if (isBaroPresent)
		{
			if((slowTick & 0x01) == 0)	//50Hz
			{
				if (baroType == BMP280)
				{
					i2cdevRead(I2C1_DEV, BMP280_I2C_ADDR, BMP280_STAT_REG, SENSORS_BARO_STATUS_LEN, &baroBuffer[0]);
					i2cdevRead(I2C1_DEV, BMP280_I2C_ADDR, BMP280_PRESSURE_MSB_REG, SENSORS_BARO_DATA_LEN, &baroBuffer[1]);
					processBarometerMeasurements(&(baroBuffer[0]));
					xQueueOverwrite(barometerDataQueue, &slowSensorData.baro);
				}
				else if (baroType == SPL06)
				{
					i2cdevRead(I2C1_DEV, SPL06_I2C_ADDR, SPL06_MODE_CFG_REG, SENSORS_BARO_STATUS_LEN, &baroBuffer[0]);
					i2cdevRead(I2C1_DEV, SPL06_I2C_ADDR, SPL06_PRESSURE_MSB_REG, SENSORS_BARO_DATA_LEN, &baroBuffer[1]);
					processBarometerMeasurements(&(baroBuffer[0]));
					xQueueOverwrite(barometerDataQueue, &slowSensorData.baro);
				}
			}
				
		}

		slowTick++;
		vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(10));	//延时10ms，100Hz
	}
}
/*获取传感器数据*/
void sensorsAcquire(sensorData_t *sensors, const u32 tick)	
{	
	sensorsReadGyro(&sensors->gyro);
	sensorsReadAcc(&sensors->acc);
	sensorsReadBaro(&sensors->baro);
}

void __attribute__((used)) EXTI4_Callback(void)
{
	portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(sensorsDataReady, &xHigherPriorityTaskWoken);

	if (xHigherPriorityTaskWoken)
	{
		portYIELD();
	}
}
/*二阶低通滤波*/
static void applyAxis3fLpf(lpf2pData *data, Axis3f* in)
{
	for (u8 i = 0; i < 3; i++) 
	{
		in->axis[i] = lpf2pApply(&data[i], in->axis[i]);
	}
}
/*传感器数据校准*/
bool sensorsAreCalibrated()	
{
	return gyroBiasFound;
}
/*上位机获取读取原始数据*/
void getSensorRawData(Axis3i16* acc, Axis3i16* gyro, Axis3i16* mag)
{
	*acc = accRaw;
	*gyro = gyroRaw;
	*mag = magRaw;
}

bool getIsMPU9250Present(void)
{
	bool value = isMPUPresent;
#ifdef SENSORS_ENABLE_MAG_AK8963
	value &= isMagPresent;
#endif
	return value;
}


bool getIsBaroPresent(void)
{
	return isBaroPresent;
}

bool sensorsIsMagPresent(void)
{
	return isMagPresent;
}



