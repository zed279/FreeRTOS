/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : adc_task.h
  * @brief          : ADC1 内部温度传感器采集任务
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __ADC_TASK_H
#define __ADC_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  ADC1 校准并创建温度采集任务 (在 MX_FREERTOS_Init 中调用)
  */
void AdcTask_Create(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADC_TASK_H */
