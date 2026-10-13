/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : btn_led_task.c
  * @brief          : 按键事件组控制 LED 任务实现
  * @note           数据流:
  *                   BTN1/BTN2 EXTI 中断 -> ISR 回调只置事件位 (osEventFlagsSet)
  *                                        -> BtnLedTask 等待事件 -> 去抖 -> 翻转 LED
  *
  *                 ISR 里能调 osEventFlagsSet 的前提:
  *                   configUSE_OS2_EVENTFLAGS_FROM_ISR = 1 (FreeRTOSConfig.h:99)
  *                   且 EXTI 中断优先级 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
  *                   (BSP 驱动配的是 5, 与上限 5 相等, 合法)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "btn_led_task.h"
#include "cmsis_os.h"
#include "main.h"
#include <stdbool.h>
/* BSP GPIO 驱动; IncludePath 中 ../Core/Inc 优先, 直接写 "gpio.h" 会命中
 * CubeMX 同名头文件 (仅 MX_GPIO_Init 原型), 故用相对路径显式包含 */
#include "../../BSP/GPIO_Driver/gpio.h"

/* Private define ------------------------------------------------------------*/
#define BTN_LED_TASK_STACK_SIZE   (128 * 4)   /* 512 字节 */
#define BTN_DEBOUNCE_MS           200U        /* 按键去抖窗口 */

/* 事件位定义: ISR 置位, 控制任务消费 */
#define BTN_EVT_BTN1              (1UL << 0)
#define BTN_EVT_BTN2              (1UL << 1)

/* Private variables ---------------------------------------------------------*/
/* 事件组仅本模块使用, 故不放入 app_ipc; 若后续其他模块也要等待按键, 再迁移 */
static osEventFlagsId_t s_btn_events;
static uint32_t         s_last_evt_tick;   /* 上次有效按键时刻 */
static bool             s_evt_valid;       /* 是否已产生过一次有效按键 */

static const osThreadAttr_t s_btn_led_task_attributes = {
  .name = "btnLedTask",
  .stack_size = BTN_LED_TASK_STACK_SIZE,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
static void BtnLedTask_Loop(void *argument);
static void Btn1_IsrCallback(gpio_pin_t pin);
static void Btn2_IsrCallback(gpio_pin_t pin);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
  * @brief  BTN1 中断回调 (中断上下文)
  * @note   只置事件位; osEventFlagsSet 在 ISR 中内部走
  *         xEventGroupSetBitsFromISR -> 定时器服务任务, 是安全的
  */
static void Btn1_IsrCallback(gpio_pin_t pin)
{
  (void)pin;
  if (s_btn_events != NULL) {
    (void)osEventFlagsSet(s_btn_events, BTN_EVT_BTN1);
  }
}

/**
  * @brief  BTN2 中断回调 (中断上下文)
  */
static void Btn2_IsrCallback(gpio_pin_t pin)
{
  (void)pin;
  if (s_btn_events != NULL) {
    (void)osEventFlagsSet(s_btn_events, BTN_EVT_BTN2);
  }
}
/* USER CODE END 0 */

void BtnLedTask_Create(void)
{
  /* LED1(PA6) / LED2(PA7): 推挽输出, 初始置高 (低电平点亮 -> 灭) */
  gpio_config_t led1_cfg = {
      .pin = GPIO_PIN_LED1, .dir = GPIO_DIR_OUTPUT, .pull = GPIO_PULL_NONE,
      .init_state = GPIO_HIGH, .speed = 0,
  };
  gpio_config_t led2_cfg = {
      .pin = GPIO_PIN_LED2, .dir = GPIO_DIR_OUTPUT, .pull = GPIO_PULL_NONE,
      .init_state = GPIO_HIGH, .speed = 0,
  };
  /* BTN1(PE3) / BTN2(PE4): 输入 + 内部上拉, 按下接 GND 产生下降沿 */
  gpio_config_t btn1_cfg = {
      .pin = GPIO_PIN_BTN1, .dir = GPIO_DIR_INPUT, .pull = GPIO_PULL_UP,
      .init_state = GPIO_HIGH, .speed = 0,
  };
  gpio_config_t btn2_cfg = {
      .pin = GPIO_PIN_BTN2, .dir = GPIO_DIR_INPUT, .pull = GPIO_PULL_UP,
      .init_state = GPIO_HIGH, .speed = 0,
  };

  if (gpio_init(&led1_cfg) != GPIO_OK) { Error_Handler(); }
  if (gpio_init(&led2_cfg) != GPIO_OK) { Error_Handler(); }
  if (gpio_init(&btn1_cfg) != GPIO_OK) { Error_Handler(); }
  if (gpio_init(&btn2_cfg) != GPIO_OK) { Error_Handler(); }

  s_btn_events = osEventFlagsNew(NULL);
  if (s_btn_events == NULL) { Error_Handler(); }

  /* 注册中断回调; port 层内部会配置 NVIC (优先级 5) 并使能 EXTI3/EXTI4 */
  if (gpio_attach_interrupt(GPIO_PIN_BTN1, GPIO_INT_FALLING, Btn1_IsrCallback) != GPIO_OK) {
      Error_Handler();
  }
  if (gpio_attach_interrupt(GPIO_PIN_BTN2, GPIO_INT_FALLING, Btn2_IsrCallback) != GPIO_OK) {
      Error_Handler();
  }

  if (osThreadNew(BtnLedTask_Loop, NULL, &s_btn_led_task_attributes) == NULL) {
      Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
static void BtnLedTask_Loop(void *argument)
{
  (void)argument;

  for(;;)
  {
    /* 阻塞等待任一按键事件; 返回时对应事件位已被自动清除 */
    uint32_t flags = osEventFlagsWait(s_btn_events,
                                      BTN_EVT_BTN1 | BTN_EVT_BTN2,
                                      osFlagsWaitAny,
                                      osWaitForever);

    uint32_t now = osKernelGetTickCount();
    if (s_evt_valid && ((uint32_t)(now - s_last_evt_tick) < BTN_DEBOUNCE_MS)) {
      continue;                     /* 去抖窗口内的重复事件直接丢弃 */
    }
    s_evt_valid     = true;
    s_last_evt_tick = now;

    if (flags & BTN_EVT_BTN1) {
      (void)gpio_toggle(GPIO_PIN_LED1);
    }
    if (flags & BTN_EVT_BTN2) {
      (void)gpio_toggle(GPIO_PIN_LED2);
    }
  }
}
/* USER CODE END 1 */
