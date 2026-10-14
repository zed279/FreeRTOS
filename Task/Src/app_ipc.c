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
/* 最新值通道要"覆盖写"(xQueueOverwrite), 那是 FreeRTOS 原生 API:
 * cmsis_os.h 只带出 FreeRTOS.h / task.h, 不带 queue.h, 故这里显式包含。
 * 不能用 osMessageQueuePut —— 它走 xQueueSendToBack, 队列满时丢的是"新值",
 * 与最新值通道需要的"丢旧值"语义正好相反。 */
#include "FreeRTOS.h"
#include "queue.h"

/* Private variables ---------------------------------------------------------*/
static osMessageQueueId_t s_temp_queue;    /* 温度多帧队列:  AdcTask -> PrintTask */
static osMessageQueueId_t s_temp_latest;   /* 温度最新值通道: AdcTask -> OledTask */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

void AppIpc_Init(void)
{
  if (s_temp_queue == NULL) {
    /* 消息按值拷贝, 深度 4、元素 sizeof(AppTempMsg_t) */
    s_temp_queue = osMessageQueueNew(APP_TEMP_QUEUE_LEN, sizeof(AppTempMsg_t), NULL);
  }

  if (s_temp_latest == NULL) {
    /* 深度必须为 1 —— 见 APP_TEMP_LATEST_LEN 的说明 */
    s_temp_latest = osMessageQueueNew(APP_TEMP_LATEST_LEN, sizeof(AppTempMsg_t), NULL);
  }

  if (s_temp_queue == NULL || s_temp_latest == NULL) {
    Error_Handler();    /* 队列创建失败 (堆不足) 属于致命错误 */
  }
}

osMessageQueueId_t AppIpc_TempQueue(void)
{
  return s_temp_queue;
}

void AppIpc_PublishTemp(const AppTempMsg_t *msg)
{
  if (msg == NULL || s_temp_latest == NULL) {
    return;
  }
  /* CMSIS 的 osMessageQueueId_t 就是内核 QueueHandle_t (cmsis_os2.c 内部直接强转),
   * 因此可以安全地把句柄交给原生 API。覆盖写对深度 1 队列永远返回 pdPASS。 */
  (void)xQueueOverwrite((QueueHandle_t)s_temp_latest, msg);
}

bool AppIpc_WaitTemp(AppTempMsg_t *msg, uint32_t timeout_ms)
{
  if (msg == NULL || s_temp_latest == NULL) {
    return false;
  }
  return (xQueueReceive((QueueHandle_t)s_temp_latest, msg,
                        pdMS_TO_TICKS(timeout_ms)) == pdPASS);
}

bool AppIpc_PeekTemp(AppTempMsg_t *msg, uint32_t timeout_ms)
{
  if (msg == NULL || s_temp_latest == NULL) {
    return false;
  }
  /* xQueuePeek: 拿到副本但元素仍留在队列里。最新值通道是"共享只读快照"——
   * 写侧靠 xQueueOverwrite 覆盖旧值, 读侧谁都不该把值拿走 (xQueueReceive
   * 会取走, 于是和 OLED 任务互相抢; 谁后到谁看到空队列)。 */
  return (xQueuePeek((QueueHandle_t)s_temp_latest, msg,
                     pdMS_TO_TICKS(timeout_ms)) == pdPASS);
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
