/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : oled_task.c
  * @brief          : OLED (SSD1315) 显示任务: 20Hz 刷新, 显示 ADC 内部温度传感器数据
  * @note           数据来源: ADC 任务的"最新值通道" (app_ipc, 深度 1 + 覆盖写)
  *
  *                 刷新策略: 固定宽度格式化 + 只推脏页。
  *                 软件 IIC @5us 半周期下推满 8 页约 150ms (最高只有 6~7Hz),
  *                 只推"真正变化的页"约 19ms/页, 20Hz 才成立。
  *
  *                 优先级给到 BelowNormal: 软件 IIC 全是忙等 (DWT 空转),
  *                 不能让它挡住采集/打印/按键这些时序敏感的活。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "oled_task.h"
#include "cmsis_os.h"
#include "main.h"
#include "app_ipc.h"
#include <stdbool.h>
#include <stdio.h>
/* BSP 驱动: 用相对路径显式包含 (与 btn_led_task.c 一致),
 * 避免与 CubeMX 生成的同名头文件相撞 */
#include "../../BSP/IIC_Driver/iic.h"
#include "../../BSP/OLED_SSD1315/oled_ssd1315.h"

/* Private define ------------------------------------------------------------*/
#define OLED_TASK_STACK_SIZE     (384 * 4)   /* 1536 字节: 本任务用 snprintf, 栈要给足 */
#define OLED_REFRESH_PERIOD_MS   50U         /* 刷新周期: 50ms = 20Hz */
#define OLED_DATA_STALE_MS       2000U       /* 超过这个时间没新数据就显示"--.-"(画面冻结会误导) */
#define OLED_SCAN_MAX_FOUND      8U          /* iic_scan 最多记录几台设备 */
#define OLED_FPS_WINDOW_MS       1000U       /* 帧率统计窗口 */

/* Private variables ---------------------------------------------------------*/
static const osThreadAttr_t s_oled_task_attributes = {
  .name = "oledTask",
  .stack_size = OLED_TASK_STACK_SIZE,
  .priority = (osPriority_t) osPriorityBelowNormal,   /* 低于采集/打印, 见文件头说明 */
};

static AppTempMsg_t s_msg;                 /* 最近一次收到的温度消息 */
static bool         s_have_data;           /* 是否收到过至少一帧 */
static uint32_t     s_last_rx_tick;        /* 最近一次收到的时刻 */

static uint32_t     s_frame_cnt;           /* 帧率统计窗口内的帧数 */
static uint32_t     s_total_frames;        /* 累计帧数 */
static uint32_t     s_fps_tick;            /* 帧率窗口起点 */
static uint32_t     s_fps10;               /* 实测刷新率 x10 */

static uint8_t      s_scan_addrs[OLED_SCAN_MAX_FOUND];
static uint8_t      s_scan_cnt;
static bool         s_oled_ok;             /* SSD1315 初始化是否成功 */
static bool         s_spare_shown;         /* 栈余量是否已显示过 */

/* Private function prototypes -----------------------------------------------*/
static void OledTask_Loop(void *argument);
static void OledTask_StaticScreen(void);
static void OledTask_Render(void);
static void OledTask_ScanCallback(uint8_t addr);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* iic_scan 回调: 中断/任务上下文都会被调, 这里只做记录 */
static void OledTask_ScanCallback(uint8_t addr)
{
  if (s_scan_cnt < OLED_SCAN_MAX_FOUND) {
    s_scan_addrs[s_scan_cnt++] = addr;
  }
}
/* USER CODE END 0 */

void OledTask_Create(void)
{
  /* 这里只做一件事: 建任务。
   *
   * 外设初始化 (iic_init / iic_scan / OLED_Init) 一律不放在这里 ——
   * Create() 由 MX_FREERTOS_Init() 在 osKernelStart() 之前调用, 那时调度器
   * 尚未启动, 任何 os* 阻塞原语都是未定义行为 (osDelay 内部无调度器状态检查,
   * 直接进 vTaskDelay -> prvAddCurrentTaskToDelayedList 操作未初始化的
   * pxCurrentTCB)。而 OLED_Init() 内部有 100ms 上电等待, 必然用到延时。
   * 详见 OledTask_Loop() 开头。 */
  if (osThreadNew(OledTask_Loop, NULL, &s_oled_task_attributes) == NULL) {
      Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
/**
  * @brief  静态内容只画一次 (启动时), 之后靠脏页机制自然保持不动
  */
static void OledTask_StaticScreen(void)
{
  char buf[24];

  OLED_ClearBuffer();   /* 只清 RAM 不推屏, 下面一次 OLED_Update 统一推 */

  OLED_ShowString(0,  0, "STM32F407 ADC TEMP",     1);
  OLED_ShowString(0, 24, "IIC SW 100k SCL=PA11",   1);
  OLED_ShowString(0, 32, "     SDA=PA12 SSD1315",  1);

  /* 总线扫描结果 */
  if (s_scan_cnt == 0U) {
    OLED_ShowString(0, 40, "iic_scan: none!  ", 1);
  } else {
    snprintf(buf, sizeof(buf), "iic_scan:0x%02X x%u",
             (unsigned)s_scan_addrs[0], (unsigned)s_scan_cnt);
    OLED_ShowString(0, 40, buf, 1);
  }

  OLED_ShowString(0, 48, s_oled_ok ? "OLED_Init: OK   " : "OLED_Init: FAIL!", 1);
}

/**
  * @brief  动态内容: 温度 / raw / 实测帧率
  * @note   全部用固定宽度格式化 —— 宽度恒定才不会留残字, 也不会左右抖动。
  *         正常值与"--.-"都是 7 字符宽 (size=2), 互相切换不残留。
  */
static void OledTask_Render(void)
{
  char buf[24];
  uint32_t now = osKernelGetTickCount();

  /* ---- 实测帧率 (用来确认真的跑在 20Hz, 而不是"以为在跑") ---- */
  s_frame_cnt++;
  s_total_frames++;
  uint32_t el = now - s_fps_tick;
  if (el >= OLED_FPS_WINDOW_MS) {
    s_fps10     = (s_frame_cnt * 10000U) / el;
    s_frame_cnt = 0;
    s_fps_tick  = now;
  }

  bool stale = (!s_have_data) ||
               ((uint32_t)(now - s_last_rx_tick) > OLED_DATA_STALE_MS);

  /* ---- 大字号温度值 ---- */
  if (stale) {
    OLED_ShowString(0, 8, "  --.-C", 2);
    snprintf(buf, sizeof(buf), "raw:----");
  } else {
    /* 用整数拆位代替 %f, 与 print_task 保持同一工程惯例 (不引浮点格式化) */
    int32_t t10 = (int32_t)(s_msg.temp_c * 10.0f);
    long    t_i = (long)(t10 / 10);
    long    t_f = (long)(t10 % 10);
    if (t_f < 0) { t_f = -t_f; }

    snprintf(buf, sizeof(buf), "%4ld.%ldC", t_i, t_f);
    OLED_ShowString(0, 8, buf, 2);

    snprintf(buf, sizeof(buf), "raw:%4u", (unsigned)s_msg.raw);
  }
  OLED_ShowString(72, 8, buf, 1);

  /* ---- 帧率 ---- */
  snprintf(buf, sizeof(buf), "fps:%2lu.%lu",
           (unsigned long)(s_fps10 / 10U), (unsigned long)(s_fps10 % 10U));
  OLED_ShowString(72, 16, buf, 1);

  /* ---- 栈余量: 跑够一段时间后再读, 此时 snprintf 已经压过栈, 读数才有意义 ---- */
  if (!s_spare_shown && (s_total_frames >= 40U)) {
    snprintf(buf, sizeof(buf), "stack spare:%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    OLED_ShowString(0, 56, buf, 1);
    s_spare_shown = true;   /* 一次性, 之后不再动这一页 */
  }
}

static void OledTask_Loop(void *argument)
{
  (void)argument;

  /* ---- 外设初始化: 必须在任务上下文里执行 ---- */
  iic_config_t cfg = {
      .bus        = IIC_BUS_OLED,
      .role       = IIC_ROLE_MASTER,
      .addr_bits  = IIC_ADDR_7BIT,
      .speed      = IIC_SPEED_STANDARD,
      .timeout_ms = 100U,
  };

  if (iic_init(&cfg) != IIC_OK) {
      Error_Handler();   /* 只可能是配置错 (总线号/引脚编码), 属代码问题 */
  }

  /* 先扫一遍总线再初始化屏幕 —— 这一步是"区分驱动问题还是接线问题"的关键:
   *   扫到 0x3C (SSD1315 常见地址) -> 接线/上拉/供电/地址全对, 问题在驱动;
   *   一个都不应答             -> 别再怀疑驱动, 去查 SDA/SCL 是否接反、
   *                              有没有上拉、模块是否供电。
   * 本驱动按开漏+内部上拉配置, 但内部上拉约 40k 偏弱, 长走线建议外部并 4.7k。 */
  s_scan_cnt = 0;
  (void)iic_scan(IIC_BUS_OLED, OledTask_ScanCallback);

  /* 初始化失败不 Error_Handler: OLED 没接不该连累 ADC/按键/串口一起停摆 */
  s_oled_ok = (OLED_Init() == 0);

  s_fps_tick = osKernelGetTickCount();

  OledTask_StaticScreen();
  /* 启动时唯一的一次全屏推送: StaticScreen 里 OLED_ClearBuffer() 把 8 页全标脏,
   * 所以这里是 8 页约 150ms (不是只有文字的 6 页) —— 面板上电时片内 RAM 是随机值,
   * 这 8 页必须推满, 推少了会留下花屏。 */
  OLED_Update();

  for (;;)
  {
    uint32_t t0 = osKernelGetTickCount();

    /* 阻塞等待最新温度值: 有数据立即返回, 无数据最多等一个刷新周期 */
    if (AppIpc_WaitTemp(&s_msg, OLED_REFRESH_PERIOD_MS)) {
      s_have_data    = true;
      s_last_rx_tick = t0;
    }

    OledTask_Render();    /* 只写显存 (RAM), 不产生 IIC 流量 */
    OLED_Update();        /* 只推脏页: 静态页不会被重复推 */

    /* 补齐到固定周期: 上面的 IIC 忙等已经占掉一部分, 剩下的用阻塞延时补足。
     * 这样既稳定 20Hz, 又不会忙等空转。 */
    uint32_t elapsed = osKernelGetTickCount() - t0;
    if (elapsed < OLED_REFRESH_PERIOD_MS) {
      osDelay(OLED_REFRESH_PERIOD_MS - elapsed);
    }
  }
}
/* USER CODE END 1 */
