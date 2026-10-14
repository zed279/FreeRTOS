# STM32F407 + FreeRTOS 工程知识要点

> **平台**：STM32F407VET6 · Keil MDK（ARMCC V5.06 update 6）· FreeRTOS V10.3.1 + CMSIS-OS2 · 软件 IIC 驱动 SSD1315 OLED
>
> 本文记录工程实践中值得复用的结论与踩过的坑。每条都对应工程里的真实代码，并标注了 HAL 源码位置，便于复核结论而不是照抄。

## 目录

- [0. 通用工程约定](#0-通用工程约定)
- [1. OLED（SSD1315）刷新](#1-oledssd1315刷新)
- [2. UART IDLE + DMA 收包](#2-uart-idle--dma-收包)
- [3. 中断与 FreeRTOS](#3-中断与-freertos)
- [4. 多任务共用外设](#4-多任务共用外设)
- [5. 症状 → 原因 排查清单](#5-症状--原因-排查清单)
- [6. 数值与预算速查](#6-数值与预算速查)
- [附录 A. HAL 源码索引](#附录-a-hal-源码索引)
- [附录 B. 静态验证手段](#附录-b-静态验证手段)

---

## 0. 通用工程约定

### 0.1 外设初始化必须放在任务上下文，不能放在 `XxxTask_Create()`

`MX_FREERTOS_Init()` 由 `main()` 在 `osKernelStart()` **之前**调用。此时调度器尚未启动，任何 `os*` 阻塞原语（`osDelay` 等）都是未定义行为——`osDelay` 内部没有"调度器是否已启动"的检查，会直接进 `vTaskDelay()` → `prvAddCurrentTaskToDelayedList()` 去操作尚未初始化的 `pxCurrentTCB`。

因此本工程的分工是：

| 函数 | 调用时机 | 允许做的事 |
|---|---|---|
| `XxxTask_Create()` | `MX_FREERTOS_Init()` 内，内核启动前 | **只** `osThreadNew()`、`osXxxNew()` 创建内核对象 |
| 任务函数开头 | 内核启动后 | `osDelay()`、启动 DMA/中断、访问外设寄存器 |

对应代码：`AdcTask_Create()` / `OledTask_Create()` / `UartCmd_TaskCreate()` 都只建任务；`OLED_Init()` 的 100ms 上电等待、`HAL_UARTEx_ReceiveToIdle_DMA()` 都在任务里执行。

### 0.2 注释用中文，字符串字面量用 ASCII

本工程注释大量使用中文，但**字符串字面量禁止出现非 ASCII 字符**，原因两条：

1. ARMCC 对非 ASCII 字面量报 `#870-D: invalid multibyte character sequence`（注释里的中文不受影响，所以这个问题只在写字面量时才暴露）；
2. 串口终端对 UTF-8 的支持参差不齐，中文帮助信息经常显示成乱码。

即：要发到串口上的字面量、命令表里的 `help` 文本，一律英文。

### 0.3 用链接 map 验证"接线"，不要靠肉眼

覆盖 HAL 的 `__weak` 回调（`HAL_UARTEx_RxEventCallback` 等）时，**写对了不代表链接器选了你的**。必须在 `.map` 里确认，成功的样子是同时出现两行：

```
stm32f4xx_hal_uart.o(i.HAL_UART_IRQHandler) refers to uart_cmd_task.o(i.HAL_UARTEx_RxEventCallback)
Removing stm32f4xx_hal_uart.o(i.HAL_UARTEx_RxEventCallback), (2 bytes).
```

`Removing ...` 这行才说明 HAL 的空实现被丢弃、你的强符号生效了。详见[附录 B](#附录-b-静态验证手段)。

---

## 1. OLED（SSD1315）刷新

### 1.1 清显存 ≠ 清屏，两个 API 语义必须分开

| API | 动作 | IIC 流量 |
|---|---|---|
| `OLED_ClearBuffer()` | `memset(GRAM)` + `DirtyMask = 0xFF` | 0（只动 RAM） |
| `OLED_Clear()` | `OLED_ClearBuffer()` + `OLED_Update()` | 推满 8 页 ≈ **150ms** |

`OLED_Clear()` 是"清屏"（要看得见），`OLED_ClearBuffer()` 是"清显存"（**渲染流程**里用）。凡是"清屏 → 画 → 刷新"的流程，第一步必须用 `OLED_ClearBuffer()`，否则会白推一整帧。本工程在头文件里把这条约定写在声明旁边：

```c
void OLED_ClearBuffer(void);  /* 只清显存不推屏: 渲染流程用这个, 避免白推一整帧 */
```

### 1.2 初始化函数不该附带推屏（开机白推一帧）

**优化前**的开机路径，同一个画面被推了两遍：

```
OLED_Init()           末尾 OLED_Clear()          → 推 8 页 ≈ 150ms
OledTask_StaticScreen()  OLED_ClearBuffer() + 画
OledTask_Loop()       OLED_Update()              → 又推 8 页 ≈ 150ms
                                                    合计 ≈ 300ms
```

第一帧的内容**马上就会被第二帧覆盖**，那 150ms 是纯浪费。改成末尾只清显存：

```c
    if (ret != 0)
        return 1;

    /* 只清显存 (RAM), 不推屏 */
    OLED_ClearBuffer();
    return 0;
}
```

**这里是"初始化函数的副作用契约"问题**：初始化函数只应把硬件和软件状态准备好，不应附带一次只有调用方才需要的数据传输。清显存后保持 `DirtyMask = 0xFF`，把"推第一帧"的责任和时机留给调用方。

### 1.3 为什么"第一帧必须推满 8 页"（这条不能省）

SSD1315 上电时**片内 GDDRAM 是随机值**，而芯片**没有"清屏"命令**——初始化命令序列里的 `0xA4`（`ENTIRE_DISPLAY_OFF`）只是让它"跟随 RAM 显示"，`0xAF`（`DISPLAY_ON`）只是打开显示，都没有清除 GDDRAM 的能力。

所以 MCU 侧第一帧必须把 8 页全部写一遍，否则面板上会残留随机花屏。**结论：这一帧不能省，只是不该推两遍。**

> 附带的一个反直觉结论：优化后随机内容的可见时间**变短了**。优化前 `DISPLAY_ON` 之后要等 150ms 的清屏帧逐页写完才不花；优化后只要等几毫秒后 `StaticScreen` 那一帧。所以去掉重复推帧不但省时间，视觉上也更好。

### 1.4 脏页机制与时间预算

```c
static uint8_t OLED_DirtyMask = 0x00;   /* bit N = 第 N 页自上次刷新后被改动过 */
```

- `OLED_SetPixel()` **只在像素真的变化时**才 `MarkDirty`（写 0 到已经是 0 的位不算改动）；
- `OLED_Update()` 开头 `if (DirtyMask == 0) return;` —— 无改动时**一字节 IIC 流量都不产生**；
- 只对脏页设列/页窗口并写 128 字节，写完清标记。

时间预算（软件 IIC @5us 半周期）：

| 项目 | 耗时 |
|---|---|
| 推 1 页（128B + 窗口命令） | ≈ 19ms |
| 推满 8 页 | ≈ 150ms |
| 全屏刷新理论上限 | 6~7Hz |

**这就是"20Hz 刷新"只在"只推脏页"的前提下才成立的原因**：静态内容（标题、接线说明）永远不脏、永远不重推，每帧真正变的只有温度那 1~2 页。

### 1.5 注意 `OLED_ClearBuffer()` 会把 8 页全标脏

`OLED_ClearBuffer()` 设的是 `DirtyMask = 0xFF`（全脏），不是"只脏我画过的页"。所以

```c
OledTask_StaticScreen();   /* 内部 ClearBuffer -> 8 页全脏 */
OLED_Update();             /* 这里推的是 8 页 ≈150ms, 不是"只有文字的 6 页" */
```

**这类"以为只推几页、实际推满"的误判会直接吃掉刷新率预算**，注释和估算都要按 8 页算。若要精确控制，得在清显存后手动收敛脏位。

---

## 2. UART IDLE + DMA 收包

### 2.1 为什么是 IDLE + DMA

| 方案 | CPU 开销 | 问题 |
|---|---|---|
| 逐字节 `RXNE` 中断 | 115200 下每秒约 **11500 次**中断 | 全是中断进出开销 |
| DMA 循环接收 | ≈ 0 | **不知道一帧何时结束** |
| **IDLE + DMA** | ≈ 0 | 总线安静一个字节时间即判定"这波发完了" |

三者合起来才是"零 CPU 开销的整包接收"：DMA 循环模式让硬件自己往内存写，CPU 完全不参与；IDLE 空闲线中断负责划分帧边界，此时读 DMA 剩余计数器就知道写到了哪。

### 2.2 核心陷阱：`Size` 是"绝对位置"，不是"新增字节数"

`HAL_UARTEx_RxEventCallback(huart, Size)` 的 `Size` 语义是 **DMA 从头开始已写入的绝对位置**（取值 `0..RxXferSize`），计算方式见 [hal_uart.c:2497-2535](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L2497-L2535)：

```c
uint16_t nb_remaining_rx_data = (uint16_t) __HAL_DMA_GET_COUNTER(huart->hdmarx);
...
huart->RxXferCount = nb_remaining_rx_data;
HAL_UARTEx_RxEventCallback(huart, (huart->RxXferSize - huart->RxXferCount));  /* = 绝对位置 */
```

循环模式下它会**回卷**，所以必须自己记录上一次搬走的位置，用 `(old_pos, pos]` 算真正的新增区间：

```c
static volatile uint16_t s_dma_old_pos;   /* 上次已搬走的位置, 0..BUF_SIZE */
```

> **把 `Size` 当新增长度直接 `memcpy`，是第一圈之内正常、第一圈之后开始错位的经典 bug。** 表现是：短命令一直好用，跑一段时间或收过一次长包之后开始出现重复/错乱字符。

### 2.3 回卷要拆成两段

```c
if (pos >= old) {
    UartCmd_PushBytes(&s_dma_buf[old], (uint16_t)(pos - old), &woken);
} else {
    /* 环形回卷: 先取 [old, 末尾), 再取 [0, pos) */
    UartCmd_PushBytes(&s_dma_buf[old], (uint16_t)(UART_CMD_DMA_BUF_SIZE - old), &woken);
    UartCmd_PushBytes(&s_dma_buf[0], pos, &woken);
}
s_dma_old_pos = pos;
```

两个必须注意的边界（HAL 的行为会造成）：

1. **位置正好回卷到 0 时**，HAL 走的是"`nb_remaining == RxXferSize`"分支，回调里传的是 `RxXferSize` 而**不是 0**（[hal_uart.c:2537-2557](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L2537-L2557)）。所以约定"位置取值 `0..BUF_SIZE`，`BUF_SIZE` 表示整圈已满"就能自洽：此时 `old_pos` 必然是上一次 TC 事件留下的 `BUF_SIZE`，`pos == old` → 新增 0 字节，正确。
2. **回卷时第一段长度可能为 0**（`old == BUF_SIZE` 时 `BUF_SIZE - old == 0`），`xStreamBufferSendFromISR()` 收到 `len == 0` 要能安全返回（本工程在 `UartCmd_PushBytes()` 开头直接 `return`）。

### 2.4 不要为了省中断而关掉 HT/TC

长期流传的优化建议是"关掉半满中断 `DMA_IT_HT` 以降低中断频率"。**在本场景下这是错的**：HT/TC 是"DMA 写满半圈/整圈"的兜底，一旦关闭，超过缓冲区长度的连续数据会在**无人察觉**的情况下被 DMA 覆盖，表现为随机丢字节。

本工程**保留** HT/TC 中断，让三种事件（IDLE / 半满 / 全满）走同一个回调，统一按绝对位置处理：

- `UART_DMARxHalfCplt` → 回调传 `RxXferSize / 2`（[hal_uart.c:3131-3149](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L3131-L3149)）
- `UART_DMAReceiveCplt` → 回调传 `RxXferSize`（[hal_uart.c:3069-3110](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L3069-L3110)）

代价是中断频率翻倍——128 字节缓冲区在 115200 下约 11ms 一圈，可以忽略。

> `HAL_DMA_Start_IT()` 是否打开 HT，取决于 `XferHalfCpltCallback` 是否非空（[hal_dma.c:451+](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c#L451)）。而 `UART_Start_Receive_DMA()` 会把它设成 `UART_DMARxHalfCplt`，所以**用 HAL 这条路径时 HT 默认就是开的**。想确认就查 map/寄存器，别猜。

### 2.5 错误回调必须区分"阻塞型"和"非阻塞型"两条路径

HAL 的 UART 错误处理有两条截然不同的路径，**混淆的后果是串口永久静默**：

| 路径 | 触发 | HAL 做了什么 | 应用该怎么办 |
|---|---|---|---|
| **阻塞型** | DMA 传输错误（TE） | 调 `UART_EndRxTransfer()`：关掉 `DMAR`/`EIE`/`IDLEIE`，`RxState = READY`，`ReceptionType = STANDARD` | **必须重新 `ReceiveToIdle_DMA()`**，否则再也不收数据 |
| **非阻塞型** | DMA 模式下跟在数据后的 ORE/FE/NE | 只回调，接收仍在跑（[hal_uart.c:2466-2480](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L2466-L2480)） | 什么都不用做；此时重启会返回 `HAL_BUSY` |

判断依据就是 `RxState`：

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1) return;
  s_stat.uart_err++;

  if (huart->RxState == HAL_UART_STATE_READY) {   /* 真的被拆掉了才重启 */
    s_dma_old_pos = 0;                            /* 重启前必须复位位置基线 */
    (void)HAL_UARTEx_ReceiveToIdle_DMA(huart, s_dma_buf, UART_CMD_DMA_BUF_SIZE);
  }
}
```

`UART_EndRxTransfer()` 的状态清理见 [hal_uart.c:3370-3385](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c#L3370-L3385)。

### 2.6 重启前必须复位位置基线

重启后 DMA 从缓冲区起点重新开始写，所以 `s_dma_old_pos` 必须同步清零。**反过来说：只有"确实重启了"才能清零**——若接收其实还在跑（非阻塞型错误）而你把基线清了，后续位置计算全部错位。

这就是上面代码把 `s_dma_old_pos = 0` 放在 `RxState == READY` 分支**内部**的原因。

**另一个容易担心的隐患其实不存在**：DMA 错误后 `hdma->ErrorCode` 是否残留、导致每次 HT/TC 都重新进错误分支、把接收反复拆掉？答案是不会——`HAL_DMA_Start_IT()` 里有 `hdma->ErrorCode = HAL_DMA_ERROR_NONE;`，重启即清。这条要查源码确认（[hal_dma.c:924-954](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c#L924-L954) 是错误路径，`HAL_DMA_Start_IT` 在 [hal_dma.c:451](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c#L451)）。

---

## 3. 中断与 FreeRTOS

### 3.1 NVIC 优先级与 `FromISR` 的硬约束

`FreeRTOSConfig.h`：

```c
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
```

含义：**抢占优先级数值 ≥ 5 的中断才允许调用 FreeRTOS 的 `FromISR` API**（数值越小优先级越高，所以 5 是"能调 FromISR 的最低优先级"，0~4 一律禁止）。

本工程的配置：

| 中断 | 优先级 | 理由 |
|---|---|---|
| `DMA2_Stream2_IRQn` | 5 | 回调里要 `xStreamBufferSendFromISR()` |
| `USART1_IRQn` | 5 | 同上 |
| `EXTI3_IRQn` / `EXTI4_IRQn` | 5 | 回调里要 `osEventFlagsSet()` |

> **CubeMX 不会替你记住这件事。** 本工程的 USART1 全局中断在 `.ioc` 里根本没勾，是在 `HAL_UART_MspInit()` 的 `USER CODE` 区手工使能的——放在 `USER CODE` 区既不会被重新生成覆盖，将来在 CubeMX 里勾上也只会重复配置同样的值，无害。

### 3.2 ISR 只搬运，不解析

IDLE 中断里**不做行解析、不查命令表、不 `printf`**，只把新增字节推进 StreamBuffer 就返回：

```
ISR: 算增量 → xStreamBufferSendFromISR → portYIELD_FROM_ISR
任务: xStreamBufferReceive → 逐字节成行 → 分词 → 查表分发
```

好处：ISR 执行时间与命令复杂度无关；解析里可以放心用 `vsnprintf`（绝不能出现在 ISR 里）。

### 3.3 同优先级中断不互相抢占 ⇒ 临界区可以省

`s_dma_old_pos` 同时被 `USART1_IRQHandler`（IDLE）和 `DMA2_Stream2_IRQHandler`（HT/TC）访问，**但两者抢占优先级相同**。Cortex-M4 的 NVIC 不允许同优先级中断互相抢占，因此对该变量的访问天然互斥，**不需要临界区**——省掉的是每次收包都要关一次中断的代价。

**这是有前提的**：一旦有人把其中一个优先级改高，立刻产生竞态。所以这条约束必须写在代码注释里：

```c
/* 只在 USART1_IRQHandler 与 DMA2_Stream2_IRQHandler 里访问 —— 两者 NVIC
 * 抢占优先级相同 (都是 5), 同优先级中断不能互相抢占, 因此本变量天然互斥。
 * 若将来把其中一个优先级改高, 这里必须补临界区。 */
static volatile uint16_t s_dma_old_pos;
```

### 3.4 字节流用 StreamBuffer，不要用消息队列

FreeRTOS 的 `StreamBuffer`（`stream_buffer.h`，实测 V10.3.1 无需额外开关，`stream_buffer.c` 已在本工程编译列表中）就是为"ISR → 任务的字节流"设计的：

```c
s_rx_stream = xStreamBufferCreate(256U, 1U);          /* 触发水平 = 1 字节 */
xStreamBufferSendFromISR(s_rx_stream, p, len, &woken); /* ISR 侧, 返回实际写入量 */
xStreamBufferReceive(s_rx_stream, chunk, sizeof(chunk), osWaitForever);  /* 任务侧 */
```

要点：

- **触发水平设 1**：命令都是短包，要的是即时性，不能为了凑批而延迟唤醒；
- **`SendFromISR` 的返回值是实际写入量**，小于请求长度说明缓冲满、多余字节被丢——必须记进统计，否则丢数据是无声的；
- 任务侧一次收 32 字节而不是 1 字节，减少唤醒次数。

`configASSERT` 在本工程是 `taskDISABLE_INTERRUPTS(); for(;;);`，所以 ISR 里对句柄做一次 NULL 判断的成本极低而收益很高（一旦走进去就是死机）。

---

## 4. 多任务共用外设

### 4.1 串口发送必须互斥，不能靠"尽量错开"

USART1 同时被 `print_task`（每秒一行周期打印）和命令任务（回显）使用。两个任务并发调 `HAL_UART_Transmit()` 的结果是**在字节层面交织**，两行都变成乱码——`HAL_UART_Transmit` 的 `__HAL_LOCK` 只保证"同时只有一个成功"，失败的返回 `HAL_BUSY` 后字节可能已经发出去一半。

正确做法：把所有 USART1 发送收敛到一个互斥量保护的函数，`print_task` 也改走它。

```c
void UartCmd_Send(const void *data, uint16_t len)
{
  if (s_tx_mutex != NULL) {
    if (osMutexAcquire(s_tx_mutex, osWaitForever) != osOK) return;
  }
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)data, len, UART_CMD_TX_TIMEOUT_MS);
  if (s_tx_mutex != NULL) (void)osMutexRelease(s_tx_mutex);
}
```

`configUSE_MUTEXES = 1` 时互斥量带优先级继承，不会因为低优先级任务持锁而把高优先级任务拖住。

> 注意 TX 与 RX 是**硬件上独立**的两条路径，RX 的 DMA 与 TX 的阻塞发送互不干扰；真正需要串行化的只有 TX。

### 4.2 队列 vs 最新值通道，`Receive` vs `Peek`

本工程同一份温度数据走两条 IPC 通道：

| 通道 | 对象 | 语义 | 消费者 |
|---|---|---|---|
| 多帧队列 | 深度 4 | 每帧都保留 | `print_task`（自己按 1 秒分频） |
| 最新值通道 | **深度必须为 1** | `xQueueOverwrite` 只留最新一帧 | `oled_task` |

深度 1 是能用 `xQueueOverwrite` 的前提（长度写 2 会在第一次调用时触发 `configASSERT` 挂死）。语义是"只保留最新值，中间帧被覆盖丢弃"——显示任务比生产端慢时，丢的应该是旧帧，屏幕永远显示最新温度。

**坑：最新值通道只能有一个 `xQueueReceive` 消费者。** 深度 1 的队列只有一份数据，再来一个消费者就是互相抢（谁后到谁看到空队列）。所以"旁观者"（如串口 `temp` 命令）必须用**非破坏性**的 `xQueuePeek`：

```c
bool AppIpc_PeekTemp(AppTempMsg_t *msg, uint32_t timeout_ms)
{
  return (xQueuePeek((QueueHandle_t)s_temp_latest, msg,
                     pdMS_TO_TICKS(timeout_ms)) == pdPASS);
}
```

等待超时要给够（本工程 100ms > ADC 的 50ms 采样周期），否则正好撞上 OLED 刚取走的那一刻会误报"没有数据"。

### 4.3 任务优先级要按"谁在忙等"来定

- 软件 IIC 全是**忙等**（DWT 空转），把 OLED 任务给 `osPriorityBelowNormal`，避免它挡住采集/打印/按键这些时序敏感的活；
- 命令任务给 `osPriorityNormal`：若给 `BelowNormal`，敲一条命令的响应会被 OLED 的忙等挡住最多一整帧（≈150ms），交互明显发卡。

---

## 5. 症状 → 原因 排查清单

| 症状 | 大概率原因 | 定位手段 |
|---|---|---|
| 串口完全收不到数据 | 强符号回调没生效（链接器仍用 HAL `__weak` 空实现）；或 NVIC 没使能 | map 里查 `Removing stm32f4xx_hal_uart.o(i.HAL_UARTEx_RxEventCallback)` 是否出现 |
| 短命令正常，跑一阵后开始出现重复/错乱字符 | 把 `Size` 当增量用了，回卷后错位 | 检查是否有 `old_pos` 与回卷拆两段 |
| 收过一次长包之后串口静默 | 阻塞型错误路径没重启接收 | map/寄存器确认 `DMAR`、`IDLEIE` 是否被关；加错误计数 |
| 随机丢字节，无任何报错 | `StreamBufferSendFromISR` 返回值没查（缓冲满静默丢弃）；或 HT/TC 被关掉 | 统计 `rx_dropped`；确认 `DMA_IT_HT` |
| 命令回显和周期打印交织成乱码 | TX 没有互斥 | 检查是否所有发送都走同一个加锁函数 |
| 开机花屏几百毫秒才正常 | 第一帧没推满 8 页 | 确认 `DirtyMask == 0xFF` 时推的是 8 页 |
| 开机白等约 150ms | 初始化函数里附带推了一帧，紧接着又被调用方推一遍 | 查 `OLED_Init()` 末尾是不是 `OLED_Clear()` |
| 刷新率上不去 | 以为只推几页、实际 `ClearBuffer()` 把 8 页全标脏 | 按 8 页 ≈150ms 重算预算 |
| 偶发硬 fault / 死机 | ISR 里调了非 `FromISR` API，或 NVIC 优先级高于 5 | 核对 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` |

---

## 6. 数值与预算速查

### 时间

| 项目 | 数值 |
|---|---|
| 软件 IIC 半周期 | 5us |
| OLED 推 1 页（128B + 窗口命令） | ≈ 19ms |
| OLED 推满 8 页 | ≈ 150ms |
| OLED 全屏刷新理论上限 | 6~7Hz |
| OLED 任务刷新周期 | 50ms（20Hz，靠只推脏页成立） |
| SSD1315 上电等待 | 100ms |
| 115200 波特率 | ≈ 11.5 字节/ms，单字节中断约 11500 次/秒 |

### 内存

| 项目 | 数值 |
|---|---|
| OLED 显存 `OLED_GRAM` | 8 页 × 128B = 1024B（`.bss`） |
| UART RX DMA 循环缓冲区 | 128B |
| UART 收包 StreamBuffer | 256B |
| UART 单行最大长度 | 96 字符 |
| FreeRTOS 堆（`heap_4`） | 15360B |
| 任务栈 | oled 1536 / uartCmd 1280 / print 1024 / adc 512 / btn 512 / default 512（字节） |
| 空闲任务栈 `configMINIMAL_STACK_SIZE` | 128 字 = 512B |
| 定时器任务栈 `configTIMER_TASK_STACK_DEPTH` | 256 字 = 1024B |
| 单个 TCB 开销 | ≈ 90~100B |

> 判断"堆够不够"不要靠估算：本工程 `info` 命令直接打印 `xPortGetFreeHeapSize()`，`stat` 命令打印任务栈余量（`uxTaskGetStackHighWaterMark()`，返回值单位是**字**，×4 才是字节）。

### 已验证的优化收益

| 优化 | 收益 |
|---|---|
| `OLED_Init()` 末尾 `OLED_Clear()` → `OLED_ClearBuffer()` | 开机省 ≈150ms |
| `OLED_Init()` 的 Max Stack Depth | 160B → 120B |
| `OLED_Clear()` 变成无调用者 | 被链接器作为未引用段丢弃 |

---

## 附录 A. HAL 源码索引

结论都基于以下位置（STM32F4xx HAL V1.8.5，工程内路径 `Drivers/STM32F4xx_HAL_Driver/Src/`）：

| 内容 | 位置 |
|---|---|
| `HAL_UARTEx_ReceiveToIdle_DMA()`：启动接收并打开 `IDLEIE` | `stm32f4xx_hal_uart.c:1781-1820` |
| `HAL_UART_IRQHandler()` 的 IDLE 分支：`Size = RxXferSize - RxXferCount` | `stm32f4xx_hal_uart.c:2482-2559` |
| IDLE 且"位置正好回卷到 0"时上报 `RxXferSize` | `stm32f4xx_hal_uart.c:2537-2557` |
| 非阻塞型错误（只回调、不拆接收） | `stm32f4xx_hal_uart.c:2466-2480` |
| `UART_DMAReceiveCplt()`（TC）回调传 `RxXferSize` | `stm32f4xx_hal_uart.c:3069-3110` |
| `UART_DMARxHalfCplt()`（HT）回调传 `RxXferSize/2` | `stm32f4xx_hal_uart.c:3131-3149` |
| `UART_Start_Receive_DMA()`：挂回调、清 ORE、开 `EIE`/`DMAR` | `stm32f4xx_hal_uart.c:3298-3349` |
| `UART_EndRxTransfer()`：关 `DMAR`/`EIE`/`IDLEIE`，`RxState = READY` | `stm32f4xx_hal_uart.c:3370-3385` |
| `UART_DMAError()`：先 `EndRxTransfer`，再回调 | `stm32f4xx_hal_uart.c:3170-3199` |
| `HAL_DMA_Start_IT()`：清 `ErrorCode`、按回调是否为空决定开 HT | `stm32f4xx_hal_dma.c:451+` |
| `HAL_DMA_IRQHandler()` 错误路径：`State = READY` 后才回调 | `stm32f4xx_hal_dma.c:924-954` |

## 附录 B. 静态验证手段

无硬件时，可用以下手段确认"代码真的生效了"，而不是靠读代码猜：

**1. 编译（Keil 命令行）**

```powershell
& "C:\Keil_v5\UV4\UV4.exe" -b "command.uvprojx" -j0 -o "build_log.txt"
```

`UV4.exe` 是 GUI 子系统程序，`&` 调用会立即返回、拿不到退出码，必须用
`Start-Process -Wait -PassThru` 再读 `ExitCode`（0=干净，1=有警告，2=有错误）。

**2. 链接 map（`MDK-ARM/command/command.map`）** —— 最有用的一份证据：

```powershell
# 强符号是否覆盖了 HAL 的 weak 实现
Select-String -Path command.map -Pattern 'HAL_UARTEx_RxEventCallback'
# 中断向量表接到了谁
Select-String -Path command.map -Pattern 'USART1_IRQHandler'
# 某个函数的完整调用链
Select-String -Path command.map -Pattern 'OLED_Init\) refers to'
# 未引用函数是否被丢弃
Select-String -Path command.map -Pattern '^\s+i\.OLED_Clear\s'
# 镜像大小
Select-String -Path command.map -Pattern 'Total (RO|RW|ROM) Size'
```

**3. 调用图报告（`MDK-ARM/command/command.htm`）**：给出每个函数的最大栈深度（`Max Depth`），用于评估任务栈是否够。注意它只反映**静态**调用链，递归和中断嵌套不在内。

**4. 新增源文件必须同时改 `.uvprojx`**：只把 `.c` 放进目录、不加进工程分组，编译会静默跳过它（链接时才报未定义符号，或者更糟——如果这个文件只是覆盖 weak 回调，链接会成功但功能完全不生效）。
