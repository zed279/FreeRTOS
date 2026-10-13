/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : led_task.c
  * @brief          : 1Hz 闪灯任务实现: LED1 (PA6, 低电平点亮) 每 500ms 翻转一次
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "led_task.h"
#include "cmsis_os.h"
#include "main.h"
/* BSP GPIO 驱动; IncludePath 中 ../Core/Inc 优先, 直接写 "gpio.h" 会命中
 * CubeMX 同名头文件 (仅 MX_GPIO_Init 原型), 故用相对路径显式包含 */
#include "../../BSP/GPIO_Driver/gpio.h"

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
#define LED_TASK_STACK_SIZE    (128 * 4)

/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
static const osThreadAttr_t s_led_task_attributes = {
  .name = "ledTask",
  .stack_size = LED_TASK_STACK_SIZE,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
static void LedTask_Loop(void *argument);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

void LedTask_Create(void)
{
  /* BSP GPIO 驱动初始化 LED1: PA6 推挽输出, 初始置高 (低电平点亮 -> 灭) */
  gpio_config_t led_cfg = {
      .pin        = GPIO_PIN_LED1,
      .dir        = GPIO_DIR_OUTPUT,
      .pull       = GPIO_PULL_NONE,
      .init_state = GPIO_HIGH,
      .speed      = 0,
  };
  if (gpio_init(&led_cfg) != GPIO_OK) {
      Error_Handler();
  }

  if (osThreadNew(LedTask_Loop, NULL, &s_led_task_attributes) == NULL) {
      Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
static void LedTask_Loop(void *argument)
{
  (void)argument;

  for(;;)
  {
    gpio_toggle(GPIO_PIN_LED1);
    osDelay(500);   /* 500ms 翻转 + 500ms 延时 = 1Hz */
  }
}
/* USER CODE END 1 */
