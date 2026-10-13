/**
 * @file    gpio_cfg.h
 * @brief   GPIO 驱动模板 - 工程配置 (STM32F407VETx + 板载 LED/按键)
 *
 * @note    本工程的引脚分配:
 *            PA6  -> LED1 (低电平点亮)
 *            PA7  -> LED2 (低电平点亮)
 *            PE3  -> 按键 1 (默认假设按下接 GND, 用内部上拉)
 *            PE4  -> 按键 2 (默认假设按下接 GND, 用内部上拉)
 *
 *          如按键实际是接 VCC, 把 PULL 改成 PULL_DOWN 即可,
 *          业务层读取逻辑无需改动 (按下时电平自动反转).
 */
#ifndef GPIO_CFG_H_
#define GPIO_CFG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 容量配置 ============================ */
#ifndef GPIO_MAX_INTERRUPT_SLOTS
#  define GPIO_MAX_INTERRUPT_SLOTS   16   /* 同时支持的最大中断引脚数 */
#endif

/* ============================ 逻辑引脚号 (工程别名) ============================ */
/* 用户可自由编号, 不要冲突即可. 推荐 0~511 之间 */
#define GPIO_PIN_LED1         0    /* PA6  低电平点亮 */
#define GPIO_PIN_LED2         1    /* PA7  低电平点亮 */
#define GPIO_PIN_BTN1         2    /* PE3  按键 1 */
#define GPIO_PIN_BTN2         3    /* PE4  按键 2 */

/* ============================ 硬件句柄编码宏 ============================ */
/**
 * @brief  将 (端口, 引脚) 编码为 32 位硬件句柄
 * @note   采用 "端口索引 << 8 | 引脚号" 的编码方案,
 *         适配 STM32 的 GPIOA~GPIOE + GPIOH 端口结构.
 */
#define GPIO_PORT_A   0
#define GPIO_PORT_B   1
#define GPIO_PORT_C   2
#define GPIO_PORT_D   3
#define GPIO_PORT_E   4
#define GPIO_PORT_F   5
#define GPIO_PORT_G   6
#define GPIO_PORT_H   7

#define HW_HANDLE(port, pin)   (((uint32_t)(port) << 8) | (uint32_t)(pin))

/* ============================ 引脚映射表 ============================ */
/**
 * @brief 硬件引脚描述结构体
 * @note  此类型在 gpio_port.h 中作为前向引用, 必须在此定义完整结构.
 *        gpio_port.h 不再重复 typedef (避免 typedef 重定义错误).
 */
typedef struct {
    uint16_t  user_pin;   /* 逻辑引脚号 */
    uint32_t  hw_handle;  /* 硬件句柄 (由 HW_HANDLE 编码) */
} gpio_hw_pin_t;

/**
 * @brief 逻辑引脚号 -> 硬件句柄 的映射表
 *
 * 对应板子接线:
 *   - LED1:  PA6  (推挽输出, 初始为高电平 -> 灭)
 *   - LED2:  PA7  (推挽输出, 初始为高电平 -> 灭)
 *   - BTN1:  PE3  (输入, 内部上拉)
 *   - BTN2:  PE4  (输入, 内部上拉)
 */
static const gpio_hw_pin_t g_gpio_pin_map[] = {
    { GPIO_PIN_LED1,   HW_HANDLE(GPIO_PORT_A, 6) },   /* PA6  */
    { GPIO_PIN_LED2,   HW_HANDLE(GPIO_PORT_A, 7) },   /* PA7  */
    { GPIO_PIN_BTN1,   HW_HANDLE(GPIO_PORT_E, 3) },   /* PE3  */
    { GPIO_PIN_BTN2,   HW_HANDLE(GPIO_PORT_E, 4) },   /* PE4  */
};

#define GPIO_PIN_TABLE_SIZE   (sizeof(g_gpio_pin_map) / sizeof(g_gpio_pin_map[0]))

#ifdef __cplusplus
}
#endif
#endif /* GPIO_CFG_H_ */
