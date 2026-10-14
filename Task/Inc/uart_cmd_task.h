/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : uart_cmd_task.h
  * @brief          : 上位机命令通道: USART1 IDLE + DMA 接收, 行解析, 命令分发
  * @note           接收链路:
  *                   上位机 -> USART1 -> DMA2_Stream2 (循环模式, 直接写内存)
  *                          -> IDLE / 半满 / 全满 中断
  *                          -> StreamBuffer (ISR 只搬运, 不解析)
  *                          -> 命令任务 (切行 / 分词 / 查表分发)
  *                 发送链路:
  *                   任意任务 -> UartCmd_Send() (互斥串行化) -> HAL_UART_Transmit
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __UART_CMD_TASK_H
#define __UART_CMD_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  创建本模块的 IPC 对象 (接收字节流缓冲 + 发送互斥量)
  * @note   在 MX_FREERTOS_Init 中、任务创建之前调用 (与 AppIpc_Init 同级)。
  *         只创建内核对象, 不碰外设 —— 外设接收在任务上下文里启动。
  */
void UartCmd_Init(void);

/**
  * @brief  创建上位机命令任务 (在 MX_FREERTOS_Init 中调用)
  * @note   任务的第一个动作就是启动 IDLE+DMA 接收, 详见 uart_cmd_task.c
  */
void UartCmd_TaskCreate(void);

/**
  * @brief  线程安全地发送原始数据 (阻塞式, 内部互斥串行化)
  * @param  data 待发送数据首地址 (不能为 NULL)
  * @param  len  字节数 (0 表示什么都不做)
  * @note   本工程所有 USART1 发送都必须走这里。否则 print_task 的周期打印
  *         会和命令回显在字节层面交织, 两边都变成乱码 —— 这是多任务共用一个
  *         串口最典型的坑, 用互斥量而不是"尽量错开"来解决。
  */
void UartCmd_Send(const void *data, uint16_t len);

/**
  * @brief  线程安全地格式化发送 (不会自动补换行, 需要时自己写 "\r\n")
  * @param  fmt printf 风格格式串
  * @note   单行超长会被截断 (内部固定大小缓冲), 不做动态分配。
  */
void UartCmd_Printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __UART_CMD_TASK_H */
