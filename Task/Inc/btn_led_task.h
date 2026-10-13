/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : btn_led_task.h
  * @brief          : 按键事件组控制 LED 任务
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __BTN_LED_TASK_H
#define __BTN_LED_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  初始化 LED/按键引脚、注册 EXTI 回调、创建事件组与控制任务
  *         (在 MX_FREERTOS_Init 中调用)
  */
void BtnLedTask_Create(void);

#ifdef __cplusplus
}
#endif

#endif /* __BTN_LED_TASK_H */
