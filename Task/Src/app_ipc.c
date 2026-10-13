/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : app_ipc.c
  * @brief          : 应用层任务间通信对象实现 (CMSIS-OS2 队列)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_ipc.h"
#include "main.h"

/* Private variables ---------------------------------------------------------*/
static osMessageQueueId_t s_temp_queue;    /* 温度数据队列: AdcTask -> PrintTask */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

void AppIpc_Init(void)
{
  if (s_temp_queue == NULL) {
    /* 消息按值拷贝, 深度 4、元素 sizeof(AppTempMsg_t) */
    s_temp_queue = osMessageQueueNew(APP_TEMP_QUEUE_LEN, sizeof(AppTempMsg_t), NULL);
  }

  if (s_temp_queue == NULL) {
    Error_Handler();    /* 队列创建失败 (堆不足) 属于致命错误 */
  }
}

osMessageQueueId_t AppIpc_TempQueue(void)
{
  return s_temp_queue;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
