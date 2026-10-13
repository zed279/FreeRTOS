/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : print_task.h
  * @brief          : 1s 串口打印任务 (USART1, 115200)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __PRINT_TASK_H
#define __PRINT_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  创建串口打印任务 (在 MX_FREERTOS_Init 中调用)
  */
void PrintTask_Create(void);

#ifdef __cplusplus
}
#endif

#endif /* __PRINT_TASK_H */
