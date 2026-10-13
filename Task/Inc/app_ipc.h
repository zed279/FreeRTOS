/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : app_ipc.h
  * @brief          : 应用层任务间通信对象集中定义 (队列 / 消息类型)
  * @note           生产者与消费者都只依赖本模块, 互不包含对方头文件,
  *                 新增 IPC 对象统一加在这里。
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __APP_IPC_H
#define __APP_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "cmsis_os.h"

/* Exported types ------------------------------------------------------------*/
/**
  * @brief  芯片内部温度传感器采样消息
  * @note   ADC 任务 -> 串口任务; 队列按值拷贝, 结构体大小 8 字节
  */
typedef struct {
  uint16_t raw;      /* ADC 原始值 (12bit, 0~4095) */
  float    temp_c;   /* 换算后的摄氏温度 */
} AppTempMsg_t;

/* Exported constants --------------------------------------------------------*/
#define APP_TEMP_QUEUE_LEN    4U    /* 队列深度: 生产者 1Hz, 消费者即时取出 */

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  创建全部 IPC 对象 (在 MX_FREERTOS_Init 中, 任务创建之前调用)
  */
void AppIpc_Init(void);

/**
  * @brief  获取温度数据队列句柄
  * @retval 队列句柄; 未初始化时为 NULL
  */
osMessageQueueId_t AppIpc_TempQueue(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_IPC_H */
