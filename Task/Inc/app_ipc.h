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
#include <stdbool.h>

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
#define APP_TEMP_QUEUE_LEN    4U    /* 队列深度: 生产者 20Hz, PrintTask 即时取出后分频打印 */

/* 最新值通道深度: 必须为 1。
 * 只有深度 1 的队列才能用 xQueueOverwrite (覆盖写); 长度写 2 会在第一次
 * 调用时触发 configASSERT 挂死。语义: 只保留最新一帧, 中间帧被覆盖丢弃。 */
#define APP_TEMP_LATEST_LEN   1U

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  创建全部 IPC 对象 (在 MX_FREERTOS_Init 中, 任务创建之前调用)
  */
void AppIpc_Init(void);

/**
  * @brief  获取温度数据队列句柄 (多帧通道: 每帧都保留, 消费端自己分频)
  * @retval 队列句柄; 未初始化时为 NULL
  */
osMessageQueueId_t AppIpc_TempQueue(void);

/**
  * @brief  发布最新温度值 (生产者调用, 非阻塞, 对深度 1 队列永远成功)
  * @param  msg  待发布的消息 (按值拷贝)
  * @note   消费端比生产端慢时 (如 OLED 刷新) 用本通道: 业务上只关心最新值,
  *         中间帧被覆盖丢弃是预期行为, 显示不会越拖越旧。
  */
void AppIpc_PublishTemp(const AppTempMsg_t *msg);

/**
  * @brief  阻塞等待一帧新的温度值 (消费者调用)
  * @param  msg        接收缓冲
  * @param  timeout_ms 超时 (ms); 0 = 不等待立即返回
  * @retval true = 取到新值; false = 超时
  * @note   队列为空时任务进入 Blocked 态让出 CPU, 一有数据就被唤醒。
  */
bool AppIpc_WaitTemp(AppTempMsg_t *msg, uint32_t timeout_ms);

/**
  * @brief  窥视最新温度值 (非破坏性: 取到副本但元素仍留在队列里)
  * @param  msg        接收缓冲
  * @param  timeout_ms 超时 (ms)
  * @retval true = 拿到值; false = 超时
  * @note   给"旁观者"用 (如串口命令 'temp'): 最新值通道只有一份数据, 用
  *         AppIpc_WaitTemp 会把它取走, 等于跟 OLED 任务抢数据; 本函数只读不取。
  */
bool AppIpc_PeekTemp(AppTempMsg_t *msg, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __APP_IPC_H */
