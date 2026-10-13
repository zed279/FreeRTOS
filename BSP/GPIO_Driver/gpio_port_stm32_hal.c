/**
 * @file    gpio_port_stm32_hal.c
 * @brief   可复用 GPIO 驱动模板 - STM32F407 HAL 移植实现
 * @note    本文件实现 gpio_port.h 中声明的 7 个 port 函数, 覆盖
 *          gpio_port.c 的弱定义桩. 适用 STM32F407VETx (LQFP100, 含 GPIOA~E+H).
 *
 *          使用 HAL 库:
 *            - 时钟: __HAL_RCC_GPIOx_CLK_ENABLE()
 *            - 配置: HAL_GPIO_Init() / HAL_GPIO_DeInit()
 *            - 输出: HAL_GPIO_WritePin()
 *            - 输入: HAL_GPIO_ReadPin()
 *            - 翻转: HAL_GPIO_TogglePin()
 *            - 中断: HAL_GPIO_Init() 配 EXTI 模式 + HAL_NVIC_EnableIRQ()
 *
 *          中断分发: 在 stm32f4xx_it.c 的 EXTIx_IRQHandler() 中调用
 *                    gpio_irq_handler(HW_HANDLE(port, pin)).
 */
#include "gpio_port.h"
#include "stm32f4xx_hal.h"   /* CubeMX 生成的 HAL 头 */

/* ============================ 内部: 句柄解码 ============================ */

/**
 * @brief  从 hw_handle 解出 GPIO 端口 (GPIOA~GPIOE + GPIOH)
 * @note   STM32F407VETx 实际可用端口: A/B/C/D/E/H (H 用于 HSE 振荡器)
 */
static GPIO_TypeDef *hw_to_port(uint32_t h)
{
    switch (h >> 8) {
        case GPIO_PORT_A: return GPIOA;
        case GPIO_PORT_B: return GPIOB;
        case GPIO_PORT_C: return GPIOC;
        case GPIO_PORT_D: return GPIOD;
        case GPIO_PORT_E: return GPIOE;
        case GPIO_PORT_H: return GPIOH;
        default:          return NULL;   /* VETx 无 F/G/I 等 */
    }
}

/**
 * @brief  从 hw_handle 解出引脚位掩码 (GPIO_PIN_0 ~ GPIO_PIN_15)
 */
static uint16_t hw_to_pin_mask(uint32_t h)
{
    return (uint16_t)(1u << (h & 0xFF));
}

/* ============================ 内部: 上下拉记录 ============================ */
/**
 * @brief  记录各引脚最近一次由 gpio_init() 设定的上下拉
 * @note   HAL_GPIO_Init() 对任何非模拟模式都会重写 PUPDR (见 HAL 源码
 *         stm32f4xx_hal_gpio.c "Activate the Pull-up or Pull down resistor"),
 *         所以中断模式下必须回填 init 阶段的上下拉, 否则依赖内部上拉/下拉的
 *         引脚 (如按键) 会悬空, 产生随机边沿中断。
 */
static gpio_pull_t s_pin_pull[8][16];   /* [端口索引][引脚号] */

/**
 * @brief  gpio_pull_t -> HAL 上下拉枚举
 */
static uint32_t hal_pull_of(gpio_pull_t pull)
{
    return (pull == GPIO_PULL_UP)   ? GPIO_PULLUP   :
           (pull == GPIO_PULL_DOWN) ? GPIO_PULLDOWN :
                                      GPIO_NOPULL;
}

/**
 * @brief  使能对应端口的 AHB1 时钟
 * @note   STM32F4 的 GPIO 都挂在 AHB1 总线上, 必须先开时钟才能写寄存器.
 */
static gpio_status_t enable_port_clock(GPIO_TypeDef *port)
{
         if (port == GPIOA) __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == GPIOB) __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == GPIOC) __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == GPIOD) __HAL_RCC_GPIOD_CLK_ENABLE();
    else if (port == GPIOE) __HAL_RCC_GPIOE_CLK_ENABLE();
    else if (port == GPIOH) __HAL_RCC_GPIOH_CLK_ENABLE();
    else return GPIO_ERR_HW;
    return GPIO_OK;
}

/* ============================ port 函数实现 ============================ */

gpio_status_t gpio_port_init(const gpio_hw_pin_t *hw, const gpio_config_t *cfg)
{
    if (hw == NULL || cfg == NULL) {
        return GPIO_ERR_PARAM;
    }

    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }

    gpio_status_t st = enable_port_clock(port);
    if (st != GPIO_OK) {
        return st;
    }

    GPIO_InitTypeDef gi = {0};
    gi.Pin   = mask;
    gi.Pull  = hal_pull_of(cfg->pull);
    gi.Speed = (cfg->speed == 0) ? GPIO_SPEED_FREQ_LOW :
                                    GPIO_SPEED_FREQ_HIGH;

    switch (cfg->dir) {
        case GPIO_DIR_INPUT:  gi.Mode = GPIO_MODE_INPUT;        break;
        case GPIO_DIR_OUTPUT: gi.Mode = GPIO_MODE_OUTPUT_PP;    break;
        case GPIO_DIR_OD:     gi.Mode = GPIO_MODE_OUTPUT_OD;    break;
        case GPIO_DIR_ANALOG: gi.Mode = GPIO_MODE_ANALOG;       break;
        default: return GPIO_ERR_PARAM;
    }
    HAL_GPIO_Init(port, &gi);

    /* 记录本引脚的上下拉, 供中断配置阶段回填 (HAL 的 IT 配置会覆盖 PUPDR) */
    s_pin_pull[hw->hw_handle >> 8][hw->hw_handle & 0xFF] = cfg->pull;

    /* 输出模式: 设置初始电平 (低电平点亮 LED, 故初始置高 = 灭) */
    if (cfg->dir == GPIO_DIR_OUTPUT || cfg->dir == GPIO_DIR_OD) {
        HAL_GPIO_WritePin(port, mask, (GPIO_PinState)cfg->init_state);
    }
    return GPIO_OK;
}

gpio_status_t gpio_port_deinit(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }
    HAL_GPIO_DeInit(port, mask);
    s_pin_pull[hw->hw_handle >> 8][hw->hw_handle & 0xFF] = GPIO_PULL_NONE;
    return GPIO_OK;
}

gpio_status_t gpio_port_set_dir(const gpio_hw_pin_t *hw, gpio_dir_t dir)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    /* 简化实现: 复用 init 路径重配方向, 上下拉保持 NOPULL, 不动电平 */
    gpio_config_t cfg = {
        .pin        = hw->user_pin,
        .dir        = dir,
        .pull       = GPIO_PULL_NONE,
        .init_state = GPIO_LOW,
        .speed      = 0,
    };
    return gpio_port_init(hw, &cfg);
}

gpio_status_t gpio_port_write(const gpio_hw_pin_t *hw, gpio_state_t state)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }
    HAL_GPIO_WritePin(port, mask, (GPIO_PinState)state);
    return GPIO_OK;
}

gpio_state_t gpio_port_read(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_LOW;
    }
    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_LOW;
    }
    return (HAL_GPIO_ReadPin(port, mask) == GPIO_PIN_SET) ? GPIO_HIGH : GPIO_LOW;
}

gpio_status_t gpio_port_toggle(const gpio_hw_pin_t *hw)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }
    /* HAL 内部已用 BSRR 寄存器实现原子翻转 */
    HAL_GPIO_TogglePin(port, mask);
    return GPIO_OK;
}

gpio_status_t gpio_port_int_config(const gpio_hw_pin_t *hw, gpio_int_mode_t mode)
{
    if (hw == NULL) {
        return GPIO_ERR_PARAM;
    }
    GPIO_TypeDef *port = hw_to_port(hw->hw_handle);
    uint16_t      mask = hw_to_pin_mask(hw->hw_handle);
    if (port == NULL) {
        return GPIO_ERR_HW;
    }

    if (mode == GPIO_INT_NONE) {
        /* 关闭 EXTI: 把引脚重配为普通输入 (保留原上下拉) */
        GPIO_InitTypeDef gi = {0};
        gi.Pin  = mask;
        gi.Mode = GPIO_MODE_INPUT;
        gi.Pull = hal_pull_of(s_pin_pull[hw->hw_handle >> 8][hw->hw_handle & 0xFF]);
        HAL_GPIO_Init(port, &gi);
        return GPIO_OK;
    }

    uint32_t exti_mode;
    switch (mode) {
        case GPIO_INT_RISING:   exti_mode = GPIO_MODE_IT_RISING;             break;
        case GPIO_INT_FALLING:  exti_mode = GPIO_MODE_IT_FALLING;            break;
        case GPIO_INT_BOTH:     exti_mode = GPIO_MODE_IT_RISING_FALLING;     break;
        case GPIO_INT_LOW:
        case GPIO_INT_HIGH:
            /* STM32 EXTI 不支持电平触发, 仅支持边沿 */
            return GPIO_ERR_UNSUPP;
        default: return GPIO_ERR_PARAM;
    }

    GPIO_InitTypeDef gi = {0};
    gi.Pin   = mask;
    gi.Mode  = exti_mode;
    /* 回填 init 阶段设定的上下拉: HAL 的 IT 配置会重写 PUPDR, 不能传 NOPULL */
    gi.Pull  = hal_pull_of(s_pin_pull[hw->hw_handle >> 8][hw->hw_handle & 0xFF]);
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &gi);

    /* 配置 NVIC, 根据 pin 号选择对应 EXTIx_IRQn
     * (STM32F4: pin 0~4 各有独立 IRQ, pin 5~9 共享 EXTI9_5, pin 10~15 共享 EXTI15_10)
     */
    IRQn_Type irqn;
    uint32_t  pin_pos = hw->hw_handle & 0xFF;
    switch (pin_pos) {
        case 0:  irqn = EXTI0_IRQn;      break;
        case 1:  irqn = EXTI1_IRQn;      break;
        case 2:  irqn = EXTI2_IRQn;      break;
        case 3:  irqn = EXTI3_IRQn;      break;
        case 4:  irqn = EXTI4_IRQn;      break;
        case 5: case 6: case 7: case 8: case 9:
                 irqn = EXTI9_5_IRQn;     break;
        case 10: case 11: case 12: case 13: case 14: case 15:
                 irqn = EXTI15_10_IRQn;  break;
        default: return GPIO_ERR_PARAM;
    }
    HAL_NVIC_SetPriority(irqn, 5, 0);
    HAL_NVIC_EnableIRQ(irqn);
    return GPIO_OK;
}
