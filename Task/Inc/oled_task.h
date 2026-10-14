/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : oled_task.h
  * @brief          : OLED (SSD1315) 显示任务: 20Hz 刷新 ADC 内部温度传感器数据
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __OLED_TASK_H
#define __OLED_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  初始化软件 IIC 总线、扫描总线、初始化 SSD1315 并创建显示任务
  *         (在 MX_FREERTOS_Init 中调用)
  */
void OledTask_Create(void);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_TASK_H */
