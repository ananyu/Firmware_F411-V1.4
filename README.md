# MiniFly STM32F411 飞控固件（深度改造版）

基于正点原子 MiniFly 开源飞控固件（STM32F411CEU6 + FreeRTOS）的个人深度改造项目，重点不在飞行算法本身，而在**工程化改造**：安全启动与 OTA、运行时零动态内存、飞行日志与失效保护。

## 主要改造内容

### 1. OTA 升级闭环（MCUboot 移植）
- 将上游 [MCUboot v2.1.0](https://github.com/mcu-tools/mcuboot) 移植到 STM32F411 裸机 Keil/AC5 环境：flash_area 后端、分区表设计、`_Static_assert` 等编译器兼容层、imgtool 签名参数链（`--align 8 -H 0x200 -S 0x1FA00`）
- 512KB Flash 分区：bootloader 128K + primary/secondary/scratch 各 128K + 配置/黑匣子 256K，槽位与 F411 不均等扇区严格对齐
- APP 侧实现 `boot_set_pending()` 升级标记与确认/回滚逻辑，两侧分区表逐字节一致
- 移植层代码见 [`Bootloader/`](Bootloader/)（bootloader 侧）与 [`MCUBoot/`](MCUBoot/)（APP 侧后端），swap 状态机与镜像校验为上游未修改代码

### 2. 运行时零动态内存分配
- 全部 FreeRTOS 任务/队列/信号量/定时器迁移至 `xXXXStatic` API，静态内存由链接器落位
- 互斥锁强制 `xSemaphoreCreateMutex`（优先级继承），二值信号量仅用于 ISR→任务事件通知
- 迁移后可从 map 文件验证堆空间整体回收，运行时零 `pvPortMalloc` 调用

### 3. 黑匣子飞行日志
- 变速采样：解锁后 10 秒 100Hz 高分辨率，巡航段降至 10Hz，failsafe 事件自动切回 100Hz 持续 5 秒
- 64K Flash 扇区约束下有效记录时长约 3 分钟，配套 PC 端导出解析脚本（`tools/blackbox_dump.py`）

### 4. 可靠性与观测
- failsafe 失效保护逻辑
- 基于 Cortex-M4 DWT CYCCNT 的任务级 CPU 占用统计：零外设占用、零额外运行时开销
- Flash 写入关中断 + 回读校验，规避静默数据损坏

## 目录结构

```
USER/          # 应用主程序与 Keil 工程
MCUBoot/       # APP 侧 MCUboot flash 后端（boot_set_pending 等）
Bootloader/    # MCUboot 移植层（bootloader 侧，详见其 README）
FWLIB/ CORE/   # ST 标准外设库 / 启动与内核文件
SYSTEM/ HARDWARE/  # 板级驱动（delay/sys/usart/led/stmflash 等）
tools/         # PC 端辅助脚本
```

## 开源声明与致谢

本仓库为学习与求职展示用途，第三方组件与其许可证如下，相关版权头在源码中全部保留：

| 组件 | 来源 | 许可证 |
|------|------|--------|
| MiniFly 飞控固件基础、板级驱动 | 正点原子（ALIENTEK）开源固件 | 见各文件版权声明（仅供学习使用） |
| MCUboot（安全启动核心） | [mcu-tools/mcuboot](https://github.com/mcu-tools/mcuboot) v2.1.0 | Apache-2.0 |
| FreeRTOS Kernel | Amazon FreeRTOS | MIT |
| STM32F4 标准外设库 | STMicroelectronics | BSD-3 |
| 部分飞控算法参考 | Cleanflight 项目 | GPL-3.0 |

改造部分（OTA 移植层、静态化迁移、黑匣子、failsafe、CPU 统计等）为本人独立编写。
