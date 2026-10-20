#ifndef __FREERTOS_STATIC_H
#define __FREERTOS_STATIC_H
/*
 * 静态内存分配辅助宏（configSUPPORT_STATIC_ALLOCATION=1）
 * 把 FreeRTOS 对象的 RAM 从运行时堆搬到链接期确定的 .bss：
 *  - 堆只剩真正动态的小对象，碎片风险归零
 *  - RAM 总占用在 map 文件里一目了然，上电即知最坏情况
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

/* 定义一个任务的静态存储（TCB + 栈，栈单位为 word）。
 * 用法：
 *   DEFINE_STATIC_TASK(radio, 150);
 *   xTaskCreateStatic(radiolinkTask, "RADIOLINK", 150, NULL, 5, stk_radio, &tcb_radio);
 * 注意：每个任务的 tag 必须唯一（宏在函数内展开为 static 局部变量，
 * 同名复用会导致后创建的任务覆盖前一个的 TCB/栈）。 */
#define DEFINE_STATIC_TASK(tag, words) \
    static StaticTask_t tcb_##tag; \
    static StackType_t  stk_##tag[words]

/* 定义一个队列的静态存储（StaticQueue_t + 存储池字节数组）。
 * 用法：
 *   DEFINE_STATIC_QUEUE(rxQ, ATKP_RX_QUEUE_SIZE, sizeof(atkp_t));
 *   rxQueue = xQueueCreateStatic(ATKP_RX_QUEUE_SIZE, sizeof(atkp_t),
 *                                sto_rxQ, &qbuf_rxQ); */
#define DEFINE_STATIC_QUEUE(tag, len, itemSize) \
    static StaticQueue_t qbuf_##tag; \
    static uint8_t       sto_##tag[(len) * (itemSize)]

/* 定义一个信号量/互斥量的静态存储。
 * 用法：
 *   DEFINE_STATIC_SEM(busy);
 *   uartBusy = xSemaphoreCreateMutexStatic(&sem_busy); */
#define DEFINE_STATIC_SEM(tag) \
    static StaticSemaphore_t sem_##tag

/* 定义一个软件定时器的静态存储。
 * 用法：
 *   DEFINE_STATIC_TIMER(ring);
 *   timer = xTimerCreateStatic("ringTimer", 50, pdTRUE, NULL, cb, &tmr_ring); */
#define DEFINE_STATIC_TIMER(tag) \
    static StaticTimer_t tmr_##tag

#endif
