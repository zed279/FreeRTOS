/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : adc_task.c
  * @brief          : ADC1 内部温度传感器采集任务实现
  * @note           流程: 软件触发单次转换 -> 轮询转换完成 -> 原始值换算摄氏度
  *                       -> 经队列送给 PrintTask 打印
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "adc_task.h"
#include "cmsis_os.h"
#include "main.h"
#include "adc.h"
#include "app_ipc.h"

/* Private define ------------------------------------------------------------*/
#define ADC_TASK_STACK_SIZE     (128 * 4)   /* 512 字节 */
#define ADC_SAMPLE_PERIOD_MS    1000U       /* 采样周期: 1Hz */
#define ADC_POLL_TIMEOUT_MS     10U         /* 转换轮询超时 (实际约 23us) */

/* 内部温度传感器换算参数 (STM32F407 数据手册典型值, 仅供参考精度)
 *   V25   = 0.76V @ 25°C
 *   斜率  = 2.5mV/°C
 * 若 Vdda 不是 3.3V, 请同步修正 ADC_VREF_V */
#define ADC_VREF_V             3.3f
#define ADC_FULL_SCALE         4096.0f      /* 12bit */
#define TS_V25_V               0.76f
#define TS_SLOPE_V_PER_C       0.0025f

/* Private variables ---------------------------------------------------------*/
static const osThreadAttr_t s_adc_task_attributes = {
  .name = "adcTask",
  .stack_size = ADC_TASK_STACK_SIZE,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
static void  AdcTask_Loop(void *argument);
static float AdcTask_RawToTempC(uint16_t raw);

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

void AdcTask_Create(void)
{
  /* 注意: STM32F4 的 HAL 未提供软件校准接口 (HAL_ADCEx_Calibration_Start 是
   * F1/F2/F3/L1/L4 系列的 API), F4 的 ADC 校准由硬件在上电后自动完成。
   * 若需更高精度, 可另采 VREFINT 通道反算实际 Vdda 修正 ADC_VREF_V。 */

  if (osThreadNew(AdcTask_Loop, NULL, &s_adc_task_attributes) == NULL) {
      Error_Handler();
  }
}

/* USER CODE BEGIN 1 */
/**
  * @brief  原始值 -> 摄氏温度
  * @note   Vsense = raw * Vref / 4096;  T = (Vsense - V25) / 斜率 + 25
  */
static float AdcTask_RawToTempC(uint16_t raw)
{
  float vsense = ((float)raw * ADC_VREF_V) / ADC_FULL_SCALE;
  return (vsense - TS_V25_V) / TS_SLOPE_V_PER_C + 25.0f;
}

static void AdcTask_Loop(void *argument)
{
  AppTempMsg_t msg;

  (void)argument;

  for(;;)
  {
    if ((HAL_ADC_Start(&hadc1) == HAL_OK) &&
        (HAL_ADC_PollForConversion(&hadc1, ADC_POLL_TIMEOUT_MS) == HAL_OK))
    {
      msg.raw    = (uint16_t)HAL_ADC_GetValue(&hadc1);
      msg.temp_c = AdcTask_RawToTempC(msg.raw);

      /* 队列满时丢弃本次采样 (timeout=0), 不让生产者阻塞 */
      (void)osMessageQueuePut(AppIpc_TempQueue(), &msg, 0U, 0U);
    }
    /* 单次转换模式, 转换结束即自动停止, 无需 HAL_ADC_Stop */

    osDelay(ADC_SAMPLE_PERIOD_MS);
  }
}
/* USER CODE END 1 */
