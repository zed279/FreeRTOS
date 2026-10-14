/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : print_task.c
  * @brief          : 串口打印任务实现: 从温度队列取数据, 经 USART1 (115200) 打印
  * @note           本任务是 ADC 温度队列的消费者; 没有数据时阻塞在队列上。
  *                 ADC 现在以 20Hz 生产, 本任务每帧都取出 (保证队列不积压),
  *                 但按 PRINT_PERIOD_MS 做时间分频, 串口仍是每秒一行。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "print_task.h"
#include "cmsis_os.h"
#include "main.h"            /* Error_Handler (原先由 usart.h 间接带出) */
#include "uart_cmd_task.h"   /* 串口发送统一走它: 内部有互斥量, 见文件末尾说明 */
#include "app_ipc.h"
#include <stdio.h>

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* 栈给到 1024 字节: 本任务用 snprintf, printf 族的栈开销较大 (512B 偏紧) */
#define PRINT_TASK_STACK_SIZE  (256 * 4)
#define PRINT_TASK_BUF_SIZE    64
/* ADC 采样 20Hz, 但串口仍按 1 行/秒输出: 既保证队列不积压, 又不刷屏。
 * 用"距上次打印的时间"分频而不是计样本数分频, 以后改 ADC 采样率不用动这里。 */
#define PRINT_PERIOD_MS        1000U

/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
static const osThreadAttr_t s_print_task_attributes = {
  .name = "printTask",
  .stack_size = PRINT_TASK_STACK_SIZE,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
static void PrintTask_Loop(void *argument);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

void PrintTask_Create(void)
{
  if (osThreadNew(PrintTask_Loop, NULL, &s_print_task_attributes) == NULL) {
      Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
static void PrintTask_Loop(void *argument)
{
  AppTempMsg_t msg;
  char buf[PRINT_TASK_BUF_SIZE];
  uint32_t last_print_tick = 0;

  (void)argument;

  for(;;)
  {
    /* 阻塞等待 ADC 任务送来的温度数据 (队列为空时让出 CPU) */
    if (osMessageQueueGet(AppIpc_TempQueue(), &msg, NULL, osWaitForever) != osOK) {
      continue;
    }

    /* 时间分频: 每帧都取走 (队列不积压), 但每秒只打印一行 */
    uint32_t now = osKernelGetTickCount();
    if ((uint32_t)(now - last_print_tick) < PRINT_PERIOD_MS) {
      continue;
    }
    last_print_tick = now;

    /* 用整数打印代替 %f: 避免浮点格式化对 C 库的依赖 (microlib 不支持 %f) */
    int32_t t10 = (int32_t)(msg.temp_c * 10.0f);
    long    t_i = (long)(t10 / 10);
    long    t_f = (long)(t10 % 10);
    if (t_f < 0) { t_f = -t_f; }

    int len = snprintf(buf, sizeof(buf), "ADC temp: raw=%u, %ld.%ld C\r\n",
                       (unsigned int)msg.raw, t_i, t_f);
    if (len > 0) {
      /* 走 UartCmd_Send 而不是直接调 HAL_UART_Transmit:
       * USART1 是 print_task 和命令任务共用的, 两边并发发送会在字节层面交织,
       * 变成两行乱码。UartCmd_Send 内部用互斥量把发送串行化。 */
      UartCmd_Send(buf, (uint16_t)len);
    }
  }
}
/* USER CODE END 1 */
