/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : uart_cmd_task.c
  * @brief          : 上位机命令通道实现 (USART1 IDLE + DMA 接收 + 行解析 + 命令分发)
  *
  * @note 为什么是 "IDLE + DMA" 而不是逐个字节收中断:
  *         - 逐个字节 RXNE 中断在 115200 下每秒 ~11500 次, 全是中断开销;
  *         - DMA 循环模式让硬件自己往缓冲区写, CPU 完全不参与;
  *         - 唯一的问题变成"怎么知道一帧收完了" —— 这就是 IDLE 空闲线检测:
  *           总线安静一个字节时间就说明这波数据发完了, 此时读 DMA 剩余计数器
  *           就知道写到了哪。三者合起来 = 零 CPU 开销的整包接收。
  *
  * @note 位置语义 (最容易写错的地方):
  *         HAL_UARTEx_RxEventCallback 给的 Size 是"DMA 从头开始已写入的绝对位置"
  *         (0..缓冲区大小), 不是"本次新增了多少字节"。循环模式下它会回卷,
  *         所以必须自己记 old_pos, 用 (old_pos, pos] 算出真正的新增区间,
  *         回卷时拆成 [old_pos, 末尾) + [0, pos) 两段。
  *         直接把 Size 当新增长度用, 是第一圈之后就开始错位的经典 bug。
  *
  * @note 中断优先级约束:
  *         USART1 与 DMA2_Stream2 的 NVIC 抢占优先级都必须是 5
  *         (= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, 能调用 FreeRTOS
  *         FromISR API 的最低优先级), 见 usart.c / dma.c。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "uart_cmd_task.h"
#include "cmsis_os.h"
#include "main.h"
#include "usart.h"
#include "app_ipc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
/* BSP GPIO 驱动: IncludePath 中 ../Core/Inc 优先, 直接写 "gpio.h" 会命中
 * CubeMX 同名头文件, 故用相对路径显式包含 (与 btn_led_task.c 一致) */
#include "../../BSP/GPIO_Driver/gpio.h"

/* Private define ------------------------------------------------------------*/
/* 栈给到 1280 字节: 本任务用 vsnprintf (printf 族栈开销大), 且分发链
 * FeedByte -> ExecLine -> Dispatch -> 命令处理 -> Printf 是嵌套调用。
 * 实测余量由 'stat' 命令的 stack hwm 打印, 不够再调这里。 */
#define UART_CMD_TASK_STACK_SIZE   (320 * 4)
#define UART_CMD_DMA_BUF_SIZE      128U   /* DMA 循环缓冲区大小 (必须 <= 65535) */
#define UART_CMD_STREAM_BUF_SIZE   256U   /* ISR -> 任务的字节流缓冲: 容纳一整行 + 余量 */
#define UART_CMD_LINE_MAX          96U    /* 单行最大字符数 (不含结束符) */
#define UART_CMD_ARG_MAX           8U     /* 单行最多几个参数 (含命令名) */
#define UART_CMD_RX_CHUNK          32U    /* 任务一次从流缓冲取多少字节 */
#define UART_CMD_TX_BUF_SIZE       128U   /* UartCmd_Printf 的单行缓冲 */
#define UART_CMD_TX_TIMEOUT_MS     200U   /* 单次阻塞发送超时 */
#define UART_CMD_PEEK_TIMEOUT_MS   100U   /* 'temp' 窥视最新值通道的等待上限 */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/**
  * @brief  接收统计 (给 'stat' 命令用)
  * @note   ISR 里写、任务里读, 故整体 volatile。32 位对齐的单字读写
  *         在 Cortex-M4 上是原子的, 不会读到半个数, 故无需临界区。
  */
typedef struct {
  uint32_t rx_events;    /* IDLE/半满/全满 事件次数 */
  uint32_t rx_bytes;     /* 累计收到的字节数 */
  uint32_t rx_dropped;   /* 流缓冲满而丢弃的字节数 */
  uint32_t uart_err;     /* USART 硬件错误次数 (ORE/FE/NE/DMA) */
  uint32_t lines;        /* 解析出的非空命令行数 */
  uint32_t unknown;      /* 未命中命令表的次数 */
} UartCmdStat_t;

typedef void (*uart_cmd_fn_t)(int argc, char **argv);

typedef struct {
  const char   *name;
  const char   *help;
  uart_cmd_fn_t fn;
} uart_cmd_entry_t;
/* USER CODE END PTD */

/* Private variables ---------------------------------------------------------*/
static const osThreadAttr_t s_uart_cmd_task_attributes = {
  .name = "uartCmdTask",
  /* 优先级 Normal: 命令回显必须及时。给 BelowNormal 的话会被 OLED 任务的
   * 软件 IIC 忙等挡住最多一整帧 (~150ms), 敲个命令要等半秒才有反应。 */
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = UART_CMD_TASK_STACK_SIZE,
};

/* ---- 接收侧 ---- */
/* DMA 循环缓冲区: 硬件直接往里写, 不经过 CPU。只有 IDLE/半满/全满三种事件
 * 会打断 CPU。真正要留足的是下面的 StreamBuffer, 因为它吸收"任务被更高优先级
 * 抢占而延迟处理"的那段时间。 */
static uint8_t s_dma_buf[UART_CMD_DMA_BUF_SIZE];

/* 上一次已经搬走的位置 (绝对下标, 取值 0..UART_CMD_DMA_BUF_SIZE)。
 * 只在 USART1_IRQHandler 与 DMA2_Stream2_IRQHandler 里访问 —— 两者 NVIC
 * 抢占优先级相同 (都是 5), 在 Cortex-M4 上同优先级中断不能互相抢占, 因此
 * 本变量天然互斥, 不需要临界区。
 * 若将来把其中一个优先级改高, 这里必须补临界区, 否则位置会错乱。 */
static volatile uint16_t s_dma_old_pos;

static StreamBufferHandle_t s_rx_stream;    /* ISR -> 任务 的单向字节流 */

/* ---- 发送侧 ---- */
static osMutexId_t s_tx_mutex;             /* USART1 发送互斥 (与 print_task 共用) */

/* ---- 行缓冲 (只在任务上下文访问) ---- */
static char   s_line[UART_CMD_LINE_MAX + 1U];
static uint8_t s_line_len;
static bool    s_line_overflow;

static volatile UartCmdStat_t s_stat;

/* Private function prototypes -----------------------------------------------*/
static void UartCmdTask_Loop(void *argument);
static void UartCmd_StartRx(void);
static void UartCmd_PushBytes(const uint8_t *p, uint16_t len, BaseType_t *woken);
static void UartCmd_FeedByte(uint8_t b);
static void UartCmd_ExecLine(void);
static void UartCmd_Dispatch(char *line);
static bool UartCmd_StrEqNoCase(const char *a, const char *b);

/* 命令处理函数 */
static void Cmd_Help(int argc, char **argv);
static void Cmd_Info(int argc, char **argv);
static void Cmd_Stat(int argc, char **argv);
static void Cmd_Temp(int argc, char **argv);
static void Cmd_Led(int argc, char **argv);
static void Cmd_Echo(int argc, char **argv);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==================== 命令表 ==================== */
/* 注意: 下面这些 help 串是"要发到串口上的字面量", 全部保持 ASCII ——
 * 本工程约定: 注释可以写中文, 但字符串字面量不放中文。
 * 原因有两条, 都是踩过的:
 *   1) ARMCC 对非 ASCII 字面量报 #870-D invalid multibyte character sequence;
 *   2) 串口终端对 UTF-8 的支持参差不齐, 中文命令帮助经常显示成乱码。
 * 新增命令只要在表里加一行 + 写一个 Cmd_Xxx 即可, 分发逻辑不用动。 */
static const uart_cmd_entry_t s_cmd_table[] = {
  { "help", "list all commands",              Cmd_Help },
  { "info", "firmware / clock / memory info", Cmd_Info },
  { "stat", "rx + parse counters (diagnose)", Cmd_Stat },
  { "temp", "read latest temperature sample", Cmd_Temp },
  { "led",  "led <1|2> <on|off|toggle>",      Cmd_Led  },
  { "echo", "echo <text> - print it back",    Cmd_Echo },
};

#define UART_CMD_COUNT  (sizeof(s_cmd_table) / sizeof(s_cmd_table[0]))

/* ==================== 接收: IDLE + DMA ==================== */

/**
  * @brief  把一段新到的字节推进 StreamBuffer (中断上下文)
  * @note   缓冲满时 xStreamBufferSendFromISR 只写入能放下的部分, 剩下的丢弃,
  *         这里把丢弃量记进统计 —— 丢字节说明任务被饿死了, 'stat' 能看出来。
  */
static void UartCmd_PushBytes(const uint8_t *p, uint16_t len, BaseType_t *woken)
{
  size_t sent;

  if (len == 0U) {
    return;
  }
  /* 正常时序下不可能为 NULL (IDLEIE 是在任务里 UartCmd_Init 之后才打开的),
   * 但中断里一旦走进去就是 xStreamBufferSendFromISR 内部 configASSERT 直接挂死,
   * 代价太大, 所以这里花一次比较兜住。 */
  if (s_rx_stream == NULL) {
    s_stat.rx_dropped += (uint32_t)len;
    return;
  }
  sent = xStreamBufferSendFromISR(s_rx_stream, p, (size_t)len, woken);
  if (sent < (size_t)len) {
    s_stat.rx_dropped += (uint32_t)((size_t)len - sent);
  }
}

/**
  * @brief  HAL 接收事件回调: IDLE 空闲线 / DMA 半满 / DMA 全满 (中断上下文)
  * @param  huart UART 句柄
  * @param  Size  事件发生时 DMA 已写入的"绝对位置" 0..UART_CMD_DMA_BUF_SIZE
  * @note   注册方式是覆盖 HAL 的 __weak 定义, 不需要改 stm32f4xx_it.c 的回调链:
  *         USART1_IRQHandler -> HAL_UART_IRQHandler -> 本函数。
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  uint16_t pos;
  uint16_t old;
  uint16_t new_bytes;
  BaseType_t woken = pdFALSE;

  if (huart->Instance != USART1) {
    return;
  }

  /* 防御: HAL 在"位置正好回卷到 0"时会按 RxXferSize 上报 (等效于"整圈已满"),
   * 与下面按 0..SIZE 处理绝对位置的约定一致, 越界值一律夹到 SIZE。 */
  pos = (Size > UART_CMD_DMA_BUF_SIZE) ? (uint16_t)UART_CMD_DMA_BUF_SIZE : Size;
  old = s_dma_old_pos;

  s_stat.rx_events++;

  if (pos >= old) {
    new_bytes = (uint16_t)(pos - old);
  } else {
    /* 环形回卷: 新增数据跨越了缓冲区末尾 */
    new_bytes = (uint16_t)(UART_CMD_DMA_BUF_SIZE - old + pos);
  }

  if (new_bytes > 0U) {
    if (pos >= old) {
      UartCmd_PushBytes(&s_dma_buf[old], (uint16_t)(pos - old), &woken);
    } else {
      UartCmd_PushBytes(&s_dma_buf[old], (uint16_t)(UART_CMD_DMA_BUF_SIZE - old), &woken);
      UartCmd_PushBytes(&s_dma_buf[0], pos, &woken);
    }
    s_stat.rx_bytes += (uint32_t)new_bytes;
  }

  s_dma_old_pos = pos;

  /* 有更高优先级任务被流缓冲唤醒时立刻切换, 别等下一个 tick */
  portYIELD_FROM_ISR(woken);
}

/**
  * @brief  UART 错误回调 (中断上下文)
  * @note   HAL 的错误路径分两类, 必须区别对待:
  *           1) 阻塞型 (DMA 错误, 或非 DMA 模式下的溢出): HAL 已经调用
  *              UART_EndRxTransfer() 把 DMAR/EIE/IDLEIE 全关掉、RxState 复位成
  *              READY —— 此时不重新拉起来, 串口就永久静默了 (最坑的一种"死"法);
  *           2) 非阻塞型 (DMA 模式下跟在数据后面的 ORE/FE/NE): HAL 只回调,
  *              接收还在跑; 此时再调一次 ReceiveToIdle_DMA 会返回 HAL_BUSY
  *              且什么都不做, 正好是我们要的。
  *         所以判断依据是 RxState: 只有真正被拆掉 (READY) 才重启,
  *         并且只有重启成功才把位置基线清零 —— 否则会和还在跑的 DMA 对不上。
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1) {
    return;
  }

  s_stat.uart_err++;

  if (huart->RxState == HAL_UART_STATE_READY) {
    s_dma_old_pos = 0;   /* DMA 从头开始, 位置基线必须同步清零 */
    (void)HAL_UARTEx_ReceiveToIdle_DMA(huart, s_dma_buf, UART_CMD_DMA_BUF_SIZE);
  }
}

/**
  * @brief  启动 IDLE+DMA 连续接收
  * @note   必须在任务上下文调用 (本工程约定: 外设初始化不在 Create() 里做,
  *         因为 Create() 由 MX_FREERTOS_Init 在 osKernelStart() 之前调用)。
  *         HAL 会顺带打开 IDLEIE 并启动循环 DMA, 之后一直有效, 不需要反复重启。
  */
static void UartCmd_StartRx(void)
{
  s_dma_old_pos = 0;
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_dma_buf, UART_CMD_DMA_BUF_SIZE) != HAL_OK) {
    Error_Handler();   /* 只可能是 DMA 配置错或 UART 处于 BUSY, 属代码问题 */
  }
}

/* ==================== 行解析 ==================== */

/**
  * @brief  喂入一个字节 (任务上下文), 负责成行
  * @note   '\r' 和 '\n' 都当行结束符: 串口助手有的发 "\n"、有的发 "\r\n"、
  *         有的发 "\r", 三种都能用; CRLF 产生的那个空行由 ExecLine 跳过。
  */
static void UartCmd_FeedByte(uint8_t b)
{
  if ((b == (uint8_t)'\n') || (b == (uint8_t)'\r')) {
    UartCmd_ExecLine();
    return;
  }

  if ((b == 0x08U) || (b == 0x7FU)) {          /* 退格 / DEL: 删掉上一个字符 */
    if (s_line_len > 0U) {
      s_line_len--;
    }
    return;
  }

  if ((b < 0x20U) || (b > 0x7EU)) {            /* 其余控制字符与非 ASCII 丢弃 */
    return;
  }

  if (s_line_len >= UART_CMD_LINE_MAX) {       /* 超长: 记标记, 本行剩下的丢掉 */
    s_line_overflow = true;
    return;
  }

  s_line[s_line_len++] = (char)b;
}

/**
  * @brief  一行结束: 校验 -> 分发
  * @note   先处理"上一行超长"和"空行", 再分词。分词会就地改写 s_line
  *         (把分隔符换成 '\0'), 所以每次成行前都要重新补结束符。
  */
static void UartCmd_ExecLine(void)
{
  const char *p;

  if (s_line_overflow) {
    s_line_len      = 0U;
    s_line_overflow = false;
    UartCmd_Printf("ERR: line too long (max %u chars)\r\n", (unsigned)UART_CMD_LINE_MAX);
    return;
  }

  s_line[s_line_len] = '\0';
  s_line_len = 0U;

  /* 跳过空行: CRLF 的第二个字符、以及只敲了回车的情况 */
  p = s_line;
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  if (*p == '\0') {
    return;
  }

  UartCmd_Dispatch(s_line);
}

/**
  * @brief  不区分大小写的字符串比较
  * @note   不用 strcasecmp: 它是 POSIX 扩展, 在 ARM 的 C 库/microlib 下
  *         不保证存在, 自己写 10 行更省事也更可控。
  */
static bool UartCmd_StrEqNoCase(const char *a, const char *b)
{
  while ((*a != '\0') && (*b != '\0')) {
    char ca = *a;
    char cb = *b;
    if ((ca >= 'A') && (ca <= 'Z')) { ca = (char)(ca - 'A' + 'a'); }
    if ((cb >= 'A') && (cb <= 'Z')) { cb = (char)(cb - 'A' + 'a'); }
    if (ca != cb) {
      return false;
    }
    a++;
    b++;
  }
  return (*a == '\0') && (*b == '\0');
}

/**
  * @brief  按空白分词并查表分发 (就地修改 line)
  */
static void UartCmd_Dispatch(char *line)
{
  char *argv[UART_CMD_ARG_MAX];
  int   argc = 0;
  char *p = line;
  size_t i;

  while ((*p != '\0') && (argc < (int)UART_CMD_ARG_MAX)) {
    while ((*p == ' ') || (*p == '\t')) {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    argv[argc++] = p;
    while ((*p != '\0') && (*p != ' ') && (*p != '\t')) {
      p++;
    }
    if (*p != '\0') {
      *p = '\0';
      p++;
    }
  }

  if (argc == 0) {
    return;
  }

  s_stat.lines++;

  for (i = 0; i < UART_CMD_COUNT; i++) {
    if (UartCmd_StrEqNoCase(argv[0], s_cmd_table[i].name)) {
      s_cmd_table[i].fn(argc, argv);
      return;
    }
  }

  s_stat.unknown++;
  UartCmd_Printf("ERR: unknown command '%s' (try 'help')\r\n", argv[0]);
}

/* ==================== 命令实现 ==================== */

static void Cmd_Help(int argc, char **argv)
{
  size_t i;

  (void)argc;
  (void)argv;

  UartCmd_Printf("commands (%u):\r\n", (unsigned)UART_CMD_COUNT);
  for (i = 0; i < UART_CMD_COUNT; i++) {
    UartCmd_Printf("  %-7s %s\r\n", s_cmd_table[i].name, s_cmd_table[i].help);
  }
}

static void Cmd_Info(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  UartCmd_Printf("build     : %s %s\r\n", __DATE__, __TIME__);
  UartCmd_Printf("hclk      : %lu Hz\r\n", (unsigned long)HAL_RCC_GetHCLKFreq());
  UartCmd_Printf("pclk2     : %lu Hz (USART1 clock)\r\n", (unsigned long)HAL_RCC_GetPCLK2Freq());
  UartCmd_Printf("uart1     : %lu 8N1\r\n", (unsigned long)huart1.Init.BaudRate);
  UartCmd_Printf("rx path   : IDLE + DMA2_Stream2 circular, buf %u B\r\n",
                 (unsigned)UART_CMD_DMA_BUF_SIZE);
  UartCmd_Printf("tasks     : %u\r\n", (unsigned)uxTaskGetNumberOfTasks());
  UartCmd_Printf("heap free : %u / %u B\r\n",
                 (unsigned)xPortGetFreeHeapSize(), (unsigned)configTOTAL_HEAP_SIZE);
}

static void Cmd_Stat(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  UartCmd_Printf("rx events : %lu (IDLE/HT/TC)\r\n", (unsigned long)s_stat.rx_events);
  UartCmd_Printf("rx bytes  : %lu\r\n", (unsigned long)s_stat.rx_bytes);
  UartCmd_Printf("rx dropped: %lu (stream buffer full)\r\n", (unsigned long)s_stat.rx_dropped);
  UartCmd_Printf("uart errs : %lu\r\n", (unsigned long)s_stat.uart_err);
  UartCmd_Printf("lines     : %lu\r\n", (unsigned long)s_stat.lines);
  UartCmd_Printf("unknown   : %lu\r\n", (unsigned long)s_stat.unknown);
  /* uxTaskGetStackHighWaterMark 返回的是"字"(StackType_t=4字节) */
  UartCmd_Printf("stack hwm : %lu B left\r\n",
                 (unsigned long)uxTaskGetStackHighWaterMark(NULL) * (unsigned long)sizeof(StackType_t));
}

static void Cmd_Temp(int argc, char **argv)
{
  AppTempMsg_t msg;
  int32_t t10;
  long    t_i;
  long    t_f;

  (void)argc;
  (void)argv;

  /* 非破坏性读取: 只窥视最新值通道, 不把值取走 (OLED 任务才是它的消费者,
   * 这里用 xQueueReceive 会跟 OLED 抢数据) */
  if (!AppIpc_PeekTemp(&msg, UART_CMD_PEEK_TIMEOUT_MS)) {
    UartCmd_Printf("temp: no sample yet\r\n");
    return;
  }

  /* 整数拆位代替 %f: 与 print_task / oled_task 保持同一工程惯例 */
  t10 = (int32_t)(msg.temp_c * 10.0f);
  t_i = (long)(t10 / 10);
  t_f = (long)(t10 % 10);
  if (t_f < 0) { t_f = -t_f; }

  UartCmd_Printf("temp: raw=%u, %ld.%ld C\r\n", (unsigned)msg.raw, t_i, t_f);
}

static void Cmd_Led(int argc, char **argv)
{
  gpio_pin_t  pin;
  gpio_state_t lvl;

  if (argc < 3) {
    UartCmd_Printf("usage: led <1|2> <on|off|toggle>\r\n");
    return;
  }

  if ((argv[1][0] == '1') && (argv[1][1] == '\0')) {
    pin = GPIO_PIN_LED1;
  } else if ((argv[1][0] == '2') && (argv[1][1] == '\0')) {
    pin = GPIO_PIN_LED2;
  } else {
    UartCmd_Printf("ERR: led index must be 1 or 2\r\n");
    return;
  }

  /* 板载 LED 低电平点亮 (见 gpio_cfg.h) */
  if (UartCmd_StrEqNoCase(argv[2], "on")) {
    (void)gpio_write(pin, GPIO_LOW);
  } else if (UartCmd_StrEqNoCase(argv[2], "off")) {
    (void)gpio_write(pin, GPIO_HIGH);
  } else if (UartCmd_StrEqNoCase(argv[2], "toggle")) {
    (void)gpio_toggle(pin);
  } else {
    UartCmd_Printf("ERR: action must be on|off|toggle\r\n");
    return;
  }

  /* 回读实际电平给出结论, 而不是"假设写成功了" */
  lvl = gpio_read(pin);
  UartCmd_Printf("LED%s = %s (pin %s)\r\n", argv[1],
                 (lvl == GPIO_LOW) ? "ON" : "OFF",
                 (lvl == GPIO_LOW) ? "low" : "high");
}

static void Cmd_Echo(int argc, char **argv)
{
  int i;

  UartCmd_Printf("echo:");
  for (i = 1; i < argc; i++) {
    UartCmd_Printf(" %s", argv[i]);
  }
  UartCmd_Printf("\r\n");
}

/* ==================== 发送 ==================== */

void UartCmd_Send(const void *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U)) {
    return;
  }

  /* 互斥量在 UartCmd_Init() 里创建。万一调用顺序被改动导致还是 NULL,
   * 退化成"不加锁直接发" —— 总比解引用空句柄崩掉强。 */
  if (s_tx_mutex != NULL) {
    if (osMutexAcquire(s_tx_mutex, osWaitForever) != osOK) {
      return;
    }
  }

  (void)HAL_UART_Transmit(&huart1, (uint8_t *)data, len, UART_CMD_TX_TIMEOUT_MS);

  if (s_tx_mutex != NULL) {
    (void)osMutexRelease(s_tx_mutex);
  }
}

void UartCmd_Printf(const char *fmt, ...)
{
  char    buf[UART_CMD_TX_BUF_SIZE];
  va_list ap;
  int     n;

  if (fmt == NULL) {
    return;
  }

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n <= 0) {
    return;
  }
  /* vsnprintf 返回的是"本该写入的长度": 被截断时要按实际容量发 */
  if (n > (int)(sizeof(buf) - 1U)) {
    n = (int)(sizeof(buf) - 1U);
  }

  UartCmd_Send(buf, (uint16_t)n);
}

/* ==================== 初始化与任务 ==================== */

void UartCmd_Init(void)
{
  if (s_rx_stream == NULL) {
    /* 触发水平 = 1 字节: 收到就唤醒任务, 不攒批 (命令都是短包, 要的是即时性) */
    s_rx_stream = xStreamBufferCreate(UART_CMD_STREAM_BUF_SIZE, 1U);
  }
  if (s_tx_mutex == NULL) {
    s_tx_mutex = osMutexNew(NULL);
  }

  if ((s_rx_stream == NULL) || (s_tx_mutex == NULL)) {
    Error_Handler();   /* 堆不足, 属致命错误 */
  }
}

void UartCmd_TaskCreate(void)
{
  if (osThreadNew(UartCmdTask_Loop, NULL, &s_uart_cmd_task_attributes) == NULL) {
    Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
static void UartCmdTask_Loop(void *argument)
{
  uint8_t chunk[UART_CMD_RX_CHUNK];

  (void)argument;

  /* 外设接收在任务上下文启动 —— 与 oled_task 的约定一致:
   * Create() 由 MX_FREERTOS_Init 在 osKernelStart() 之前调用, 那时中断里
   * xStreamBufferSendFromISR 唤醒任务还没有意义。 */
  UartCmd_StartRx();

  UartCmd_Printf("\r\n[uart] IDLE+DMA RX ready @ %lu 8N1, type 'help'\r\n",
                 (unsigned long)huart1.Init.BaudRate);

  for (;;)
  {
    /* 一次尽量多取几个字节: 减少唤醒次数, 但触发水平是 1, 不会为了凑批而延迟 */
    size_t n = xStreamBufferReceive(s_rx_stream, chunk, sizeof(chunk), osWaitForever);

    for (size_t i = 0; i < n; i++) {
      UartCmd_FeedByte(chunk[i]);
    }
  }
}
/* USER CODE END 1 */
