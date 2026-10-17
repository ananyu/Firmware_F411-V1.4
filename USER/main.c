#include "system.h"	/*头文件集合*/
#include "bootutil/bootutil_public.h"  /*MCUBoot公共API：确认当前镜像，防止回滚*/
#include "ota.h"
#include "ota_uart_bridge.h"

/********************************************************************************
 * 本程序只供学习使用，未经作者许可，不得用于其它任何用途
 * ALIENTEK MiniFly
 * main.c
 * 包括系统初始化和创建任务
 * 正点原子@ALIENTEK
 * 技术论坛:www.openedv.com
 * 创建日期:2017/5/12
 * 版本：V1.3
 * 版权所有，盗版必究。
 * Copyright(C) 广州市星翼电子科技有限公司 2014-2024
 * All rights reserved
********************************************************************************/

TaskHandle_t startTaskHandle;
//static 主要是为了限制函数的作用域，使其只能在当前文件中被调用，防止其他文件误调用该函数导致错误。
static void startTask(void *arg);

int main()
{
    systemInit();			/*底层硬件初始化*/

    xTaskCreate(startTask, "START_TASK", 300, NULL, 2, &startTaskHandle);	/*创建起始任务*/

    vTaskStartScheduler();	/*开启任务调度*/

    while(1) {};
}
/*创建任务*/
void startTask(void *arg)
{
    taskENTER_CRITICAL();	 
    /*进入临界区，因为创建任务的时候并不运行，
    而实际运行是在vTaskStartScheduler()中，所以在这个任务里面实际运行的时候系统已经开始调度了，
    所以为了防止被高层级的抢占，需要进入临界区*/

    boot_set_confirmed();												/*确认当前slot0镜像，防止下次上电被回滚(MCUBoot)*/

    xTaskCreate(radiolinkTask, "RADIOLINK", 150, NULL, 5, NULL);		/*创建无线连接任务*/

    xTaskCreate(usblinkRxTask, "USBLINK_RX", 150, NULL, 4, NULL);		/*创建usb接收任务*/
    xTaskCreate(usblinkTxTask, "USBLINK_TX", 150, NULL, 3, NULL);		/*创建usb发送任务*/

    xTaskCreate(atkpTxTask, "ATKP_TX", 150, NULL, 3, NULL);				/*创建atkp发送任务任务*/
    xTaskCreate(atkpRxAnlTask, "ATKP_RX_ANL", 300, NULL, 6, NULL);		/*创建atkp解析任务*/

    xTaskCreate(configParamTask, "CONFIG_TASK", 150, NULL, 2, NULL);	/*创建参数配置任务*/

    xTaskCreate(pmTask, "PWRMGNT", 150, NULL, 2, NULL);					/*创建电源管理任务*/

    xTaskCreate(sensorsTask, "SENSORS", 450, NULL, 4, NULL);			/*创建传感器处理任务*/

    xTaskCreate(stabilizerTask, "STABILIZER", 450, NULL, 5, NULL);		/*创建姿态任务*/

    xTaskCreate(expModuleMgtTask, "EXP_MODULE", 150, NULL, 1, NULL);	/*创建扩展模块管理任务*/

    xTaskCreate(otaTask, "OTA", 300, NULL, 4, NULL);					/*创建OTA升级处理任务*/
    xTaskCreate(otaUartBridgeTask, "OTA_UART", 200, NULL, 3, NULL);	/*创建OTA串口桥任务*/
    otaInit();

    printf("Free heap: %d bytes\n", xPortGetFreeHeapSize());			/*打印剩余堆栈大小*/

    vTaskDelete(startTaskHandle);										/*删除开始任务*/

    taskEXIT_CRITICAL();	/*退出临界区*/
}


//空闲任务钩子函数，由freertos auto call，
void vApplicationIdleHook( void )
{
    static u32 tickWatchdogReset = 0;

    portTickType tickCount = getSysTickCnt();
//这处看门狗喂狗设计可能有问题，从代码理解中，他是超过看门狗时间就喂狗，
// 但是超过的时间设置的也是看门狗重置的时间
    if (tickCount - tickWatchdogReset > WATCHDOG_RESET_MS)
    {
        tickWatchdogReset = tickCount;
        watchdogReset();
    }

    __WFI();	/*进入低功耗模式*/
}
















