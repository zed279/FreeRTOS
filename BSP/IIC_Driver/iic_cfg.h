/**
 * @file    iic_cfg.h
 * @brief   IIC 驱动模板 - 工程配置 (STM32F407VET6 + SSD1315 OLED)
 *
 * @note    本工程配置:
 *            IIC_BUS_OLED  -> 软件 IIC, 用 PA11(SCL) + PA12(SDA), 100kbps, 接 SSD1315 OLED
 *
 *          ⚠ 注意: 本文件从 F103 模板复制而来, 原模板用的是 PA6/PA7;
 *            本工程 PA6/PA7 是 LED1/LED2 且 led_task 一直在驱动, 直接沿用会
 *            与 LED 抢引脚。此处已改为 PA11/PA12 (实测未被其他外设占用)。
 *
 *          port 层 iic_port_stm32_hal.c 会根据 hw_handle 的 bus_type 位区分:
 *            bit 31 = 0 : 软件位时序, 低 30 位为 SCL/SDA 引脚句柄编码
 *            bit 31 = 1 : 硬件 I2C, 低 30 位为硬件实例索引 (本工程未使用)
 *
 *          移植到新工程只需修改本文件的:
 *            1. IIC_BUS_x 宏 (业务别名)
 *            2. HW_IIC_SOFT(scl, sda) / HW_IIC_HARD(idx) 编码宏
 *            3. g_iic_bus_map[] 映射表
 *          无需修改 iic.c / iic_port.h 通用层代码。
 */
#ifndef IIC_CFG_H_
#define IIC_CFG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============================ 容量配置 ============================ */
#ifndef IIC_MAX_BUSES
#  define IIC_MAX_BUSES        4    /* 同时支持的最大总线数 */
#endif

/* 软件 IIC 默认时序半周期 (us), 100kbps 周期 10us, 半周期 5us */
#ifndef IIC_SOFT_HALF_PERIOD_US
#  define IIC_SOFT_HALF_PERIOD_US   5
#endif

/* 软件 IIC 总线空闲/复位最长等待 (us), 用于检测 SCL 钳低 */
#ifndef IIC_SOFT_SCL_TIMEOUT_US
#  define IIC_SOFT_SCL_TIMEOUT_US   1000
#endif

/* ============================ 逻辑总线号 (工程别名) ============================ */
#define IIC_BUS_OLED           0    /* OLED 屏幕, 软件 IIC, 接 SSD1315 */

/* ============================ 硬件句柄编码宏 ============================ */
/**
 * @brief  硬件句柄编码方案
 * @note   bit31 = bus_type: 0=软件位时序, 1=硬件 I2C
 *         软件: bit[30:16] = SCL 引脚句柄, bit[15:0] = SDA 引脚句柄
 *               (引脚句柄编码方式与 GPIO 驱动一致: port<<8 | pin)
 *         硬件: bit[7:0]  = I2C 实例索引 (0=I2C1, 1=I2C2, 2=I2C3, ...)
 */
#define IIC_BUS_TYPE_SOFT   0x00000000u
#define IIC_BUS_TYPE_HARD   0x80000000u

/* 软件 IIC 句柄编码 (scl_pin/ sda_pin 用 GPIO 驱动的 HW_HANDLE(port, pin) 编码) */
#define HW_IIC_SOFT(scl_hw_handle, sda_hw_handle)   \
        (IIC_BUS_TYPE_SOFT | ((uint32_t)(scl_hw_handle) << 16) | ((uint32_t)(sda_hw_handle) & 0xFFFFu))

/* 硬件 I2C 句柄编码 (instance_index: 0=I2C1, 1=I2C2, ...) */
#define HW_IIC_HARD(instance_index)   \
        (IIC_BUS_TYPE_HARD | (uint32_t)(instance_index))

/* ============================ 硬件引脚描述结构体 ============================ */
/**
 * @brief 逻辑总线 -> 硬件句柄 的映射条目
 * @note   此类型必须在 iic_cfg.h 定义, 因为映射表 g_iic_bus_map[] 需要它,
 *         iic_port.h 仅做前向引用以避免 typedef 重定义。
 */
typedef struct {
    uint16_t  bus_id;       /* 逻辑总线号 (与 iic_bus_t 一致) */
    uint32_t  hw_handle;    /* 硬件句柄 (由 HW_IIC_SOFT/HW_IIC_HARD 编码) */
} iic_hw_bus_t;

/* ============================ 总线映射表 ============================ */
/**
 * @brief 逻辑总线号 -> 硬件句柄 的映射表
 *
 * 对应板子接线:
 *   - OLED (软件 IIC):  SCL = PA11, SDA = PA12
 *
 * @note  端口/引脚号编码沿用 GPIO 驱动约定 (port<<8 | pin):
 *        PA11 = (0 << 8) | 11 = 0x000B
 *        PA12 = (0 << 8) | 12 = 0x000C
 *
 *        上拉说明: port 层把 SCL/SDA 配成开漏 + 内部上拉 (约 40kΩ)。
 *        内部上拉偏弱, 若模块板上已有 4.7k~10k 上拉则最稳; 若走线较长
 *        出现偶发 NACK, 优先在 SCL/SDA 上各并一颗 4.7k 到 3V3。
 */
#define HW_HANDLE_GPIO(port, pin)  (((uint32_t)(port) << 8) | (uint32_t)(pin))
#define GPIO_PORT_A 0   /* 沿用 GPIO 驱动约定 */

static const iic_hw_bus_t g_iic_bus_map[] = {
    /* OLED: SCL=PA11, SDA=PA12  (顺序不要写反 —— SDA/SCL 接反是 OLED 全黑的头号原因) */
    { IIC_BUS_OLED,    HW_IIC_SOFT(HW_HANDLE_GPIO(GPIO_PORT_A, 11), HW_HANDLE_GPIO(GPIO_PORT_A, 12)) },
};

#define IIC_BUS_TABLE_SIZE   (sizeof(g_iic_bus_map) / sizeof(g_iic_bus_map[0]))

#ifdef __cplusplus
}
#endif
#endif /* IIC_CFG_H_ */
