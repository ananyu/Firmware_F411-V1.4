# MiniFly PX4-Style Sensor Refactor Guide
# MiniFly 传感器按 PX4 思路重构说明

## 1. Goal
## 1. 目标

This document explains how to migrate the current MiniFly sensor architecture toward a PX4-style structure.
本文说明如何把当前 MiniFly 的传感器架构，逐步改造成接近 PX4 的结构。

The final target is:
最终目标是：

- `acc/gyro` use `FIFO + high-rate trigger`
- `acc/gyro` 使用 `FIFO + 高频触发`
- `imu temperature` is read at low rate
- `IMU 温度` 低频读取
- `mag/baro` become independent slow paths
- `磁力计/气压计` 变成独立的低频路径
- `STM32` reads slow sensors directly through `bypass`
- `STM32` 通过 `bypass` 直接读取慢速传感器
- the estimator no longer assumes all sensors update at the same time
- 姿态/状态估算不再假设所有传感器同步更新

This is not only a design note; it is intended to be a practical migration tutorial.
这不只是设计说明，而是一份可以照着一步步修改的迁移教程。

## 2. One-Sentence Core Idea
## 2. 一句话抓住核心

Let the IMU fast path serve only attitude propagation, and let slow sensors update asynchronously on their own cadence.
让 IMU 高频路径只服务姿态传播，慢速传感器按自己的频率异步更新。

If you remember only one thing, remember this:
如果你只记住一句话，就记住这个：

Stop treating all sensors as one synchronous packet.
不要再把所有传感器当成“一个同步大包”。

## 3. Current MiniFly Architecture
## 3. 当前 MiniFly 架构

The current project roughly works like this:
当前工程大致是这样工作的：

1. `MPU9250` generates a data-ready interrupt.
1. `MPU9250` 产生数据就绪中断。
2. `sensorsTask()` wakes up.
2. `sensorsTask()` 被唤醒。
3. The code reads one big register block starting at `ACCEL_XOUT_H`.
3. 代码从 `ACCEL_XOUT_H` 开始读取一大块寄存器。
4. This block contains:
4. 这一块数据里面包含：
- `acc/gyro`
- `加速度计/陀螺仪`
- `EXT_SENS_DATA` shadow for `AK8963`
- `AK8963` 映射到 `EXT_SENS_DATA` 的数据
- `EXT_SENS_DATA` shadow for `BMP280`
- `BMP280` 映射到 `EXT_SENS_DATA` 的数据
5. These values are processed and pushed into queues.
5. 然后把这些值处理后推入队列。
6. `stabilizer.c` calls `sensorsAcquire()`.
6. `stabilizer.c` 调用 `sensorsAcquire()`。
7. `sensfusion6.c` uses fixed-`dt` `imuUpdate(acc, gyro, ...)`.
7. `sensfusion6.c` 用固定 `dt` 的 `imuUpdate(acc, gyro, ...)` 做融合。

### 3.1 Why this feels convenient
### 3.1 为什么这种结构一开始会觉得方便

Everything looks unified.
因为看起来所有传感器都被统一管理了。

One interrupt, one read, one packet, one update loop.
一个中断，一次读取，一个数据包，一个更新循环。

### 3.2 Why this becomes a problem
### 3.2 为什么后面会变成问题

This convenience hides several architectural issues:
这种“方便”背后其实藏着几个结构性问题：

1. `mag/baro` are forced into the IMU fast path.
1. `磁力计/气压计` 被强行绑进了 IMU 高频路径。
2. Slow sensors are checked much more often than they really update.
2. 慢速传感器会被远高于自身更新频率地反复检查。
3. `WAIT_FOR_ES`, slave delay, and `EXT_SENS_DATA` couple unrelated timing domains.
3. `WAIT_FOR_ES`、从机 delay、`EXT_SENS_DATA` 把本来不相关的时间域耦合在一起。
4. The estimator tends to assume one synchronized sensor frame.
4. 估算器会逐渐形成“所有传感器同步到来”的假设。
5. Any jitter on the IMU path directly disturbs fast-path timing.
5. IMU 读取链路上的抖动会直接影响高频路径的时间均匀性。

## 4. What PX4 Does Differently
## 4. PX4 的思路有什么不同

After checking the local PX4 tree at `PX4-Autopilot-main`, the key ideas are:
我对本地 `PX4-Autopilot-main` 做了对应检查，核心思路如下：

### 4.1 IMU is FIFO-centric
### 4.1 IMU 路径以 FIFO 为核心

PX4 does not rely on “one interrupt = one sample = one direct register burst” as its main design.
PX4 并不是把“一个中断 = 一个样本 = 一次直接寄存器突发读取”当成主设计。

Its main idea is:
它的主思路是：

- let the sensor accumulate samples in FIFO
- 让传感器先在 FIFO 里积累样本
- wake up periodically or by trigger
- 再由周期调度或触发唤醒
- drain all complete samples from FIFO
- 一次性取走所有完整样本
- process them in batch
- 批量处理

This reduces sensitivity to service-time jitter.
这样可以降低对软件响应时延抖动的敏感性。

### 4.2 Temperature is low-rate
### 4.2 温度是低频路径

PX4 reads IMU temperature at low rate, around `1 Hz`.
PX4 对 IMU 温度是低频读取的，大约 `1 Hz`。

It is not part of every fast batch.
它不属于每一帧高速批处理的一部分。

### 4.3 Temperature is useful, not decorative
### 4.3 温度不是摆设，而是有用途的

In PX4, temperature can be used by the thermal-compensation module.
在 PX4 中，温度可以被温漂补偿模块使用。

That means low-rate temperature reads still have engineering value.
这意味着即使低频读取温度，它仍然有工程价值。

### 4.4 Slow sensors are independent
### 4.4 慢速传感器是独立路径

The important lesson is not “copy every PX4 file”.
这里的重点不是“照抄 PX4 每个文件”。

The important lesson is:
真正应该学的是：

- fast IMU transport is independent
- 高频 IMU 传输路径独立
- magnetometer update is independent
- 磁力计更新独立
- barometer update is independent
- 气压计更新独立
- the estimator fuses them according to freshness and purpose
- 估算器根据数据的新鲜度和用途去融合它们

## 5. Recommended Target Architecture
## 5. 推荐的目标架构

### 5.1 Fast path
### 5.1 高频路径

- `MPU9250` only serves `acc/gyro`
- `MPU9250` 只负责 `加速度计/陀螺仪`
- enable `FIFO`
- 开启 `FIFO`
- use interrupt or timer as a wakeup source to drain FIFO
- 用中断或定时器作为“去清 FIFO”的唤醒源
- publish only IMU fast data on this path
- 这条路径只发布 IMU 高频数据

### 5.2 Slow path
### 5.2 低频路径

- disable MPU auxiliary proxy mode during runtime
- 运行时关闭 MPU 的外部从机代理模式
- enable `bypass`
- 开启 `bypass`
- let `STM32` directly read `AK8963` and `BMP280`
- 让 `STM32` 直接读取 `AK8963` 和 `BMP280`
- run them in a separate periodic task
- 用独立的周期任务处理它们

### 5.3 Estimator path
### 5.3 估算器路径

- gyro drives attitude propagation
- 陀螺仪驱动姿态传播
- accel participates in fast correction
- 加速度计参与快速修正
- mag only corrects yaw when new data arrives
- 磁力计仅在有新数据时修正航向
- baro only updates altitude/vertical state when new data arrives
- 气压计仅在有新数据时更新高度/垂直状态

## 6. Current vs PX4 vs Target
## 6. 当前方案 vs PX4 vs 目标方案

| Item | Current MiniFly | PX4 Idea | Recommended Target |
| --- | --- | --- | --- |
| IMU read mode | direct burst read | FIFO batch read | FIFO batch read |
| IMU 读取方式 | 直接大块寄存器读取 | FIFO 批量读取 | FIFO 批量读取 |
| Slow sensors | proxied by MPU9250 | independent path | direct STM32 read via bypass |
| 慢速传感器 | 由 MPU9250 代理 | 独立路径 | 通过 bypass 由 STM32 直读 |
| Interrupt meaning | read one packet now | wake service logic | wake and drain FIFO |
| 中断含义 | 现在读一个整包 | 唤醒服务逻辑 | 唤醒并清空 FIFO |
| Estimator assumption | all sensors arrive together | asynchronous fusion | asynchronous fusion |
| 估算器假设 | 所有传感器同步到来 | 异步融合 | 异步融合 |
| Temperature | buried in fast path or ignored | low-rate path | low-rate path |
| 温度 | 混在快路径或被忽略 | 低频路径 | 低频路径 |

## 7. Why Bypass Is the Better Slow-Path Choice
## 7. 为什么慢速路径更适合用 bypass

When `bypass` is enabled:
当 `bypass` 开启时：

- `MPU9250` stops acting as the runtime proxy for external sensors
- `MPU9250` 不再作为外部传感器的运行时代理
- `STM32` sees the slow sensors directly on the bus
- `STM32` 可以直接在总线上看到这些慢速传感器

This is better because:
这样做更合适，因为：

1. no `SLVx` scheduling limitation
1. 不再受 `SLVx` 调度限制
2. no `I2C_MST_DLY` dependency
2. 不再依赖 `I2C_MST_DLY`
3. no `WAIT_FOR_ES` coupling
3. 不再被 `WAIT_FOR_ES` 耦合
4. clearer timestamps and ownership
4. 时间戳和数据归属更清晰
5. easier debugging
5. 更容易调试

### Important clarification
### 一个很重要的概念澄清

`bypass = true` does **not** mean “STM32 reads through MPU9250”.
`bypass = true` **不是** “STM32 通过 MPU9250 中转读取”。

It means the external bus is exposed, so the MCU reads the devices directly.
它的意思是外部总线被放出来了，因此 MCU 是直接读取这些器件。

## 8. Suggested Migration Order
## 8. 建议的迁移顺序

Do not change everything in one shot.
不要一口气全部改掉。

The safest order is:
最稳妥的顺序是：

1. split fast and slow responsibilities in software
1. 先在软件结构上拆分快慢职责
2. move IMU to FIFO
2. 再把 IMU 改成 FIFO
3. remove `mag/baro` from `EXT_SENS_DATA` runtime path
3. 把 `磁力计/气压计` 从 `EXT_SENS_DATA` 运行路径里移走
4. enable `bypass`
4. 开启 `bypass`
5. add direct `AK8963/BMP280` reads
5. 增加直接读取 `AK8963/BMP280`
6. change estimator to asynchronous fusion style
6. 最后把估算器改成异步融合风格

## 9. Detailed Step-by-Step Tutorial
## 9. 详细修改教程

## Step 1. Split responsibilities conceptually first
## 第 1 步：先在概念上拆分快慢职责

Relevant files:
相关文件：

- `FLIGHT/src/sensors.c`
- `FLIGHT/src/stabilizer.c`
- `FLIGHT/src/sensfusion6.c`
- `FLIGHT/src/state_estimator.c`

Do not treat `sensorData_t` as one perfectly synchronized packet anymore.
不要再把 `sensorData_t` 当成一个完全同步的大包。

Instead, think in two groups:
而是拆成两组来理解：

- fast group: `gyro + acc + imu temperature`
- 快速组：`陀螺仪 + 加速度计 + IMU 温度`
- slow group: `mag + baro`
- 慢速组：`磁力计 + 气压计`

Practical first step:
实际上的第一步可以先这样做：

- let the fast producer update only IMU queues
- 让快路径生产者只更新 IMU 队列
- let the slow producer update mag/baro queues
- 让慢路径生产者更新磁力计/气压计队列

At this stage, you do not need to redesign every public structure yet.
在这个阶段，不需要一开始就重构所有公开结构体。

## Step 2. Remove runtime dependency on `SLV0/1/2 + EXT_SENS_DATA`
## 第 2 步：移除运行时对 `SLV0/1/2 + EXT_SENS_DATA` 的依赖

Current behavior:
当前行为：

- `SLV0` reads `AK8963`
- `SLV0` 读取 `AK8963`
- `SLV1/SLV2` read `BMP280`
- `SLV1/SLV2` 读取 `BMP280`
- the result appears in `EXT_SENS_DATA`
- 然后结果映射到 `EXT_SENS_DATA`

Target behavior:
目标行为：

- `SLV0/1/2` are no longer used for normal runtime updates
- `SLV0/1/2` 不再用于正常运行时更新
- `EXT_SENS_DATA` is no longer the normal source of mag/baro data
- `EXT_SENS_DATA` 不再是磁力计/气压计的正常数据来源

That means these old helper paths should gradually disappear from runtime:
也就是说，运行时要逐步摆脱这些旧路径：

- `mpu9250SetSlaveAddress()`
- `mpu9250SetSlaveRegister()`
- `mpu9250SetSlaveDataLength()`
- `processMagnetometerMeasurements()` on shadow data
- 基于 shadow data 的 `processMagnetometerMeasurements()`
- `processBarometerMeasurements()` on shadow data
- 基于 shadow data 的 `processBarometerMeasurements()`

## Step 3. Enable bypass for the slow path
## 第 3 步：给慢速路径开启 bypass

Register-level idea:
寄存器层面的核心思路：

- `mpu9250SetI2CMasterModeEnabled(false)`
- `mpu9250SetI2CMasterModeEnabled(false)`
- `mpu9250SetI2CBypassEnabled(true)`
- `mpu9250SetI2CBypassEnabled(true)`

This means:
这表示：

- MPU auxiliary master is disabled
- MPU 内部辅助主机关闭
- the external sensor bus is exposed to the MCU
- 外部传感器总线直接暴露给 MCU

Suggested helper:
建议增加一个帮助函数：

- `sensorsSetupSlowSensorBypass()`
- `sensorsSetupSlowSensorBypass()`

Suggested sequence:
建议顺序：

1. disable old slave channels
1. 先关掉旧的 slave 通道
2. disable MPU master mode
2. 再关掉 MPU master 模式
3. enable bypass
3. 然后开启 bypass

## Step 4. Add direct `AK8963` reads
## 第 4 步：加入直接读取 `AK8963`

Add a function such as:
增加一个类似下面的函数：

- `sensorsReadMagDirect()`
- `sensorsReadMagDirect()`

Suggested sequence:
建议读取顺序：

1. read `AK8963_ST1`
1. 先读 `AK8963_ST1`
2. if `DRDY == 0`, return
2. 如果 `DRDY == 0`，直接返回
3. read `HXL...HZH` and `ST2`
3. 读取 `HXL...HZH` 以及 `ST2`
4. check overflow/status bits
4. 检查溢出和状态位
5. convert and publish
5. 转换并发布

### About update rate
### 关于更新频率

`AK8963` itself can be configured to `8 Hz` or `100 Hz` in continuous mode.
`AK8963` 自己可以配置成连续模式 `8 Hz` 或 `100 Hz`。

That setting is inside the magnetometer's own control register, not inside MPU slave delay.
这个频率是在磁力计自己的控制寄存器里配置的，不是在 MPU 的 slave delay 里配置的。

So remember:
所以要记住：

- `AK8963 mode` decides how often new mag data is generated
- `AK8963 mode` 决定磁力计自己多久产生一笔新数据
- your polling loop only decides how often you go check whether new data is available
- 你的轮询循环只决定你多久去检查一次有没有新数据

## Step 5. Add direct `BMP280` reads
## 第 5 步：加入直接读取 `BMP280`

Add a function such as:
增加一个类似下面的函数：

- `sensorsReadBaroDirect()`
- `sensorsReadBaroDirect()`

Suggested sequence:
建议顺序：

1. read `BMP280_STAT_REG`
1. 读取 `BMP280_STAT_REG`
2. if conversion is busy, skip
2. 如果转换还没完成，就跳过
3. read 6 bytes from pressure/temp registers
3. 从压力/温度寄存器读取 6 字节
4. build raw values
4. 拼出原始值
5. run compensation
5. 做补偿计算
6. publish pressure, temperature, altitude
6. 发布压力、温度、高度

## Step 6. Convert the IMU fast path to FIFO
## 第 6 步：把 IMU 高频路径改成 FIFO

Current behavior:
当前行为：

- interrupt arrives
- 中断到来
- read one direct register burst
- 直接读一次寄存器大块数据

Target behavior:
目标行为：

- interrupt or periodic trigger arrives
- 中断或周期触发到来
- read `FIFO_COUNT`
- 读取 `FIFO_COUNT`
- drain all complete frames from `FIFO_R_W`
- 从 `FIFO_R_W` 取出所有完整帧
- parse and publish them
- 解析并发布它们

### 6.1 Add FIFO setup
### 6.1 增加 FIFO 初始化

Create a helper like:
建议增加类似函数：

- `sensorsSetupImuFifo()`
- `sensorsSetupImuFifo()`

Inside it:
内部建议做这些事：

1. disable FIFO first
1. 先关闭 FIFO
2. reset FIFO
2. 复位 FIFO
3. enable accel FIFO
3. 开启加速度 FIFO
4. enable gyro FIFO
4. 开启陀螺仪 FIFO
5. do not include external sensor bytes
5. 不要把外部传感器字节塞进 FIFO
6. enable FIFO in user control
6. 在 user control 中开启 FIFO

### 6.2 Define frame size clearly
### 6.2 明确 FIFO 帧大小

For `acc + gyro`, one frame is usually:
对于 `加速度 + 陀螺仪`，一帧通常是：

- accel xyz: `6 bytes`
- 加速度 xyz：`6 字节`
- gyro xyz: `6 bytes`
- 陀螺仪 xyz：`6 字节`

Total:
总计：

- `12 bytes`
- `12 字节`

### 6.3 Redefine interrupt meaning
### 6.3 重新定义中断的含义

After FIFO is enabled, do not think:
开启 FIFO 后，不要再认为：

- “interrupt means read one sample now”
- “中断来了就立刻读一个样本”

Think instead:
要改成：

- “interrupt means there is work to do; drain FIFO”
- “中断来了说明该处理了；去清 FIFO”

This is one of the biggest mindset changes.
这是整个改造里非常关键的一个思维转换。

## Step 7. Add a slow periodic task
## 第 7 步：增加慢速周期任务

Create a dedicated task, for example:
单独建立一个任务，例如：

- `slowSensorsTask()`
- `slowSensorsTask()`

Suggested startup rate:
建议初始调度频率：

- wake at `100 Hz`
- 以 `100 Hz` 唤醒

Inside the loop:
循环内部建议：

1. call `sensorsReadMagDirect()` every cycle
1. 每个周期都调用 `sensorsReadMagDirect()`
2. call `sensorsReadBaroDirect()` every `2` or `4` cycles
2. 每 `2` 或 `4` 个周期调用一次 `sensorsReadBaroDirect()`

Suggested starting values:
建议起步值：

- mag service loop: `100 Hz`
- 磁力计服务循环：`100 Hz`
- baro service loop: `25 Hz` or `50 Hz`
- 气压计服务循环：`25 Hz` 或 `50 Hz`

### Why polling at 100 Hz is not a conflict
### 为什么 100 Hz 轮询并不冲突

If `AK8963` itself updates at `100 Hz`, then polling it at `100 Hz` is reasonable.
如果 `AK8963` 自己配置成 `100 Hz` 更新，那么你以 `100 Hz` 去轮询它是合理的。

If the IMU fast path runs at `200 Hz` or more, that is also fine.
如果 IMU 高频路径跑在 `200 Hz` 或更高，也完全没问题。

These are different paths.
因为这已经是两条不同路径了。

The key is that the 200 Hz IMU loop no longer drags the mag path with it.
关键点在于：`200 Hz` 的 IMU 路径不再把磁力计路径硬拽着一起跑。

## Step 8. Add low-rate IMU temperature read
## 第 8 步：增加低频 IMU 温度读取

Create a function such as:
增加一个函数，例如：

- `sensorsReadImuTemperature()`
- `sensorsReadImuTemperature()`

Call it at around:
调用频率建议：

- `1 Hz`
- `1 Hz`

Reasons:
原因：

1. temperature changes slowly
1. 温度变化很慢
2. it should not burden the fast path
2. 不值得占用快路径
3. it can later support thermal compensation
3. 后续可以支撑温漂补偿

## Step 9. Reshape the estimator like PX4 logic
## 第 9 步：把估算器思路改得更像 PX4

This part matters as much as the sensor driver changes.
这一部分和驱动改动同样重要。

If you only change the transport path but keep the old “all sensors synchronized” estimator assumption, the gain is limited.
如果你只改传输路径，却保留“所有传感器同步”的旧估算假设，收益会很有限。

### 9.1 Fast propagation is driven by gyro
### 9.1 高频传播由陀螺仪驱动

The estimator update cadence should follow gyro samples.
估算器更新节奏应该跟着陀螺仪样本走。

In practice:
实际意义是：

- every new IMU sample batch updates attitude propagation
- 每批新的 IMU 样本都更新姿态传播
- you do not wait for mag or baro
- 不等待磁力计或气压计

### 9.2 Accel stays in the fast correction loop
### 9.2 加速度计保留在快速修正环

Accel is still useful at high rate for gravity-direction correction.
加速度计在高频下仍然适合用于重力方向修正。

So it should stay with the fast IMU loop.
因此它应该保留在 IMU 高频循环里。

### 9.3 Mag becomes asynchronous yaw correction
### 9.3 磁力计变成异步航向修正

When a new mag sample arrives:
当有新的磁力计样本到来时：

- use it to correct yaw
- 用它修正航向
- do not force a full synchronized “sensor frame”
- 不要强行凑一个“完整同步传感器帧”

This is much closer to PX4 thinking.
这就更接近 PX4 的思路了。

### 9.4 Baro serves altitude, not the fast attitude loop
### 9.4 气压计服务高度，不参与高速姿态环

Baro should feed altitude or vertical estimation.
气压计应该进入高度或垂直方向估计。

It should not slow down or structure the fast attitude loop.
它不应该拖慢或塑造高速姿态循环的结构。

### 9.5 Minimal safe transition plan for estimator code
### 9.5 估算器最稳妥的过渡方案

Do not rewrite the whole estimator at once.
不要一上来就把整个估算器全重写。

Safer transition:
更稳妥的过渡方式：

1. keep `imuUpdate(acc, gyro, ...)` as the fast function
1. 先保留 `imuUpdate(acc, gyro, ...)` 作为高频函数
2. stop requiring mag/baro freshness in the same cycle
2. 不再要求同一周期里 mag/baro 也必须同步新鲜
3. add separate hooks such as `magUpdate(...)` and `baroUpdate(...)`
3. 增加独立的 `magUpdate(...)`、`baroUpdate(...)` 之类接口
4. only after this becomes stable, consider deeper estimator refactor
4. 等稳定后，再考虑更深入的估算器重构

## 10. Practical File-by-File Modification Order
## 10. 按文件的实际修改顺序

To reduce risk, modify in this order:
为了降低风险，建议按这个顺序改：

1. `FLIGHT/src/sensors.c`
1. `FLIGHT/src/sensors.c`
- split fast/slow tasks
- 拆分快慢任务
- add FIFO IMU path
- 增加 FIFO IMU 路径
- remove `EXT_SENS_DATA` runtime dependence
- 去掉运行时对 `EXT_SENS_DATA` 的依赖

2. `HARDWARE/src/mpu9250.c` and related headers
2. `HARDWARE/src/mpu9250.c` 及相关头文件
- add FIFO helpers if missing
- 如果缺少 FIFO 帮助函数就补上
- add FIFO count / overflow helpers
- 补充 FIFO count / overflow 接口

3. `HARDWARE/src/ak8963.c`
3. `HARDWARE/src/ak8963.c`
- confirm direct runtime reads in bypass mode
- 确认 bypass 模式下可以直接运行时读取

4. `HARDWARE/src/bmp280.c`
4. `HARDWARE/src/bmp280.c`
- confirm periodic direct polling
- 确认周期直读轮询逻辑

5. `FLIGHT/src/stabilizer.c`
5. `FLIGHT/src/stabilizer.c`
- remove the assumption that all sensors are acquired together
- 去掉“所有传感器一起取到”的假设

6. `FLIGHT/src/sensfusion6.c`
6. `FLIGHT/src/sensfusion6.c`
- keep fast propagation on IMU updates
- 让高速传播继续跟着 IMU 更新
- accept asynchronous mag/baro updates
- 接受异步的 mag/baro 更新

## 11. Verification Checklist
## 11. 验证清单

### Phase A. FIFO-only IMU verification
### 阶段 A：只验证 IMU FIFO

Check:
检查：

1. FIFO count grows and drains correctly
1. FIFO count 能正常增长并被清空
2. no overflow in normal runtime
2. 正常运行下没有溢出
3. gyro/acc queues update steadily
3. 陀螺仪/加速度队列更新稳定
4. attitude remains stable using only fast IMU data
4. 只靠高速 IMU 数据时姿态仍然稳定

### Phase B. Slow direct reads
### 阶段 B：验证慢速直读

Check:
检查：

1. `AK8963` can be read after bypass is enabled
1. 开启 bypass 后 `AK8963` 能正常读取
2. `BMP280` can be read after bypass is enabled
2. 开启 bypass 后 `BMP280` 能正常读取
3. IMU fast path remains unaffected
3. IMU 高频路径不受影响

### Phase C. Asynchronous fusion runtime
### 阶段 C：验证异步融合运行态

Check:
检查：

1. attitude loop is smooth
1. 姿态循环平滑
2. yaw only changes when fresh mag data arrives
2. 航向只在新的磁力计数据到来时被修正
3. baro updates at low rate without disturbing fast attitude updates
3. 气压计低频更新但不会扰动高速姿态更新
4. CPU load is cleaner or lower than before
4. CPU 负载比之前更干净或者更低

## 12. Common Mistakes
## 12. 常见错误

1. Do not keep `WAIT_FOR_ES` enabled once slow sensors are no longer part of the fast path.
1. 一旦慢速传感器不再属于快路径，就不要继续开着 `WAIT_FOR_ES`。
2. Do not leave `SLV0/1/2` runtime proxy logic enabled together with bypass direct access.
2. 不要让 `SLV0/1/2` 运行时代理逻辑和 bypass 直读同时存在。
3. Do not keep reading one large combined register block after FIFO is enabled.
3. 开启 FIFO 后，不要继续沿用“一次读一个大寄存器包”的思路。
4. Do not assume `mag/baro` update on every IMU cycle.
4. 不要假设 `磁力计/气压计` 每个 IMU 周期都更新。
5. Do not rewrite the full estimator and transport path in one commit.
5. 不要在一次提交里把传输层和估算器一起全部重写。

## 13. Recommended Milestones
## 13. 建议分阶段里程碑

### Milestone 1
### 里程碑 1

This is the safest first milestone:
这是最稳妥的第一阶段目标：

1. keep current estimator math mostly unchanged
1. 尽量保持现有估算数学主体不变
2. move `acc/gyro` to FIFO
2. 把 `acc/gyro` 改成 FIFO
3. move `mag/baro` to direct bypass reads
3. 把 `mag/baro` 改成 bypass 直读
4. keep slow sensor queues independent
4. 让慢速传感器队列独立运行

This alone already brings most of the structural improvement.
仅仅完成这一阶段，其实已经拿到了大部分结构收益。

### Milestone 2
### 里程碑 2

After milestone 1 is stable:
等里程碑 1 稳定以后：

1. add low-rate IMU temperature handling
1. 加入低频 IMU 温度处理
2. add explicit timestamps for slow-path data
2. 为慢速路径数据加入明确时间戳
3. refactor estimator consumers to fully respect asynchronous updates
3. 继续重构估算器使用方，让它完整接受异步更新

## 14. Final Conclusion
## 14. 最终结论

The PX4-style direction for this MiniFly project is:
这个 MiniFly 工程按 PX4 思路演进，最终方向就是：

- `acc/gyro`: FIFO-driven fast path
- `acc/gyro`：FIFO 驱动的高频路径
- `temperature`: low-rate auxiliary path
- `temperature`：低频辅助路径
- `mag/baro`: independent slow path
- `mag/baro`：独立的慢速路径
- `bypass`: enabled for direct STM32 access to slow sensors
- `bypass`：开启后由 STM32 直接访问慢速传感器

The most important conceptual upgrade is:
最关键的概念升级是：

IMU drives propagation, mag corrects yaw when fresh, and baro corrects altitude when fresh.
IMU 驱动传播，磁力计在有新数据时修正航向，气压计在有新数据时修正高度。

That is much closer to PX4, and it is also a cleaner long-term architecture for your MiniFly codebase.
这才真正更接近 PX4，也更适合作为你这个 MiniFly 工程的长期结构。
