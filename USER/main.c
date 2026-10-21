#include "system.h"	/*头文件集合*/
#include "bootutil/bootutil_public.h"  /*MCUBoot公共API：确认当前镜像，防止回滚*/
#include "freertos_static.h"	/*静态内存分配辅助宏*/
#include "fs_stats.h"	/*运行时间统计+栈高水位上报*/
#include "blackbox.h"	/*S4 flight-log blackbox*/
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
    DEFINE_STATIC_TASK(start, 300);

    systemInit();			/*底层硬件初始化*/

    startTaskHandle = xTaskCreateStatic(startTask, "START_TASK", 300, NULL, 2, stk_start, &tcb_start);	/*创建起始任务（静态）*/

    vTaskStartScheduler();	/*开启任务调度*/

    while(1) {};
}
/*创建任务*/
void startTask(void *arg)
{
    /*所有任务改用静态创建：TCB+栈在链接期落进 .bss，不再占用运行时堆*/
    DEFINE_STATIC_TASK(radio, 150);
    DEFINE_STATIC_TASK(usbRx, 150);
    DEFINE_STATIC_TASK(usbTx, 150);
    DEFINE_STATIC_TASK(atkpTx, 150);
    DEFINE_STATIC_TASK(atkpRx, 300);
    DEFINE_STATIC_TASK(cfg, 150);
    DEFINE_STATIC_TASK(pm, 150);
    DEFINE_STATIC_TASK(sensors, 450);
    DEFINE_STATIC_TASK(stab, 450);
    DEFINE_STATIC_TASK(expMgt, 150);
    DEFINE_STATIC_TASK(ota, 300);
    DEFINE_STATIC_TASK(otaUart, 200);
    DEFINE_STATIC_TASK(bbx, 150);

    taskENTER_CRITICAL();
    /*进入临界区，因为创建任务的时候并不运行，
    而实际运行是在vTaskStartScheduler()中，所以在这个任务里面实际运行的时候系统已经开始调度了，
    所以为了防止被高层级的抢占，需要进入临界区*/

    boot_set_confirmed();												/*确认当前slot0镜像，防止下次上电被回滚(MCUBoot)*/

    blackboxInit();														/*S4黑匣子：校验/格式化+恢复写游标（可能擦除64K扇区约2s，IWDG已放宽）*/

    xTaskCreateStatic(radiolinkTask, "RADIOLINK", 150, NULL, 5, stk_radio, &tcb_radio);		/*创建无线连接任务*/

    xTaskCreateStatic(usblinkRxTask, "USBLINK_RX", 150, NULL, 4, stk_usbRx, &tcb_usbRx);		/*创建usb接收任务*/
    xTaskCreateStatic(usblinkTxTask, "USBLINK_TX", 150, NULL, 3, stk_usbTx, &tcb_usbTx);		/*创建usb发送任务*/

    xTaskCreateStatic(atkpTxTask, "ATKP_TX", 150, NULL, 3, stk_atkpTx, &tcb_atkpTx);			/*创建atkp发送任务任务*/
    xTaskCreateStatic(atkpRxAnlTask, "ATKP_RX_ANL", 300, NULL, 6, stk_atkpRx, &tcb_atkpRx);	/*创建atkp解析任务*/

    xTaskCreateStatic(configParamTask, "CONFIG_TASK", 150, NULL, 2, stk_cfg, &tcb_cfg);		/*创建参数配置任务*/

    xTaskCreateStatic(pmTask, "PWRMGNT", 150, NULL, 2, stk_pm, &tcb_pm);						/*创建电源管理任务*/

    xTaskCreateStatic(sensorsTask, "SENSORS", 450, NULL, 4, stk_sensors, &tcb_sensors);		/*创建传感器处理任务*/

    xTaskCreateStatic(stabilizerTask, "STABILIZER", 450, NULL, 5, stk_stab, &tcb_stab);		/*创建姿态任务*/

    xTaskCreateStatic(expModuleMgtTask, "EXP_MODULE", 150, NULL, 1, stk_expMgt, &tcb_expMgt);	/*创建扩展模块管理任务*/

    xTaskCreateStatic(otaTask, "OTA", 300, NULL, 4, stk_ota, &tcb_ota);						/*创建OTA升级处理任务*/
    xTaskCreateStatic(otaUartBridgeTask, "OTA_UART", 200, NULL, 3, stk_otaUart, &tcb_otaUart);	/*创建OTA串口桥任务*/
    otaInit();

    xTaskCreateStatic(blackboxTask, "BLACKBOX", 150, NULL, 1, stk_bbx, &tcb_bbx);				/*创建黑匣子写任务（1ms摊薄编程1字）*/

    fsStatsStart();																				/*创建运行时间/栈水位统计任务*/

    printf("Free heap: %d bytes\n", xPortGetFreeHeapSize());			/*打印剩余堆栈大小*/

    vTaskDelete(startTaskHandle);										/*删除开始任务*/

    taskEXIT_CRITICAL();	/*退出临界区*/
}


/*静态分配模式（configSUPPORT_STATIC_ALLOCATION=1）要求应用为
 * 空闲任务和定时器任务提供 TCB/栈内存（vTaskStartScheduler 内部调用）*/
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    DEFINE_STATIC_TASK(idle, configMINIMAL_STACK_SIZE);
    *ppxIdleTaskTCBBuffer = &tcb_idle;
    *ppxIdleTaskStackBuffer = stk_idle;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

#if configUSE_TIMERS
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    DEFINE_STATIC_TASK(tmrTask, configTIMER_TASK_STACK_DEPTH);
    *ppxTimerTaskTCBBuffer = &tcb_tmrTask;
    *ppxTimerTaskStackBuffer = stk_tmrTask;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
#endif

//空闲任务钩子函数，由freertos auto call，
void vApplicationIdleHook( void )
{
    static u32 tickWatchdogReset = 0;

    portTickType tickCount = getSysTickCnt();
//这处看门狗喂狗设计可能有问题，从代码理解中，他是超过看门狗时间就喂狗，
// 但是超过的时间设置的也是看门狗重置的时间
    if (tickCount - tickWatchdogReset > WATCHDOG_FEED_INTERVAL_MS)
    {
        tickWatchdogReset = tickCount;
        watchdogReset();
    }

    __WFI();	/*进入低功耗模式*/
}
















