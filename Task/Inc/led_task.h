/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : led_task.h
  * @brief          : 1Hz 闪灯任务 (LED1 / PA6, 经 BSP GPIO 驱动控制)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __LED_TASK_H
#define __LED_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  初始化 LED1 引脚并创建闪灯任务 (在 MX_FREERTOS_Init 中调用,
  *         调度器启动前完成, 与 CubeMX 外设初始化时机一致)
  */
void LedTask_Create(void);

#ifdef __cplusplus
}
#endif

#endif /* __LED_TASK_H */
