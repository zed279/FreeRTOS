#ifndef __OLED_SSD1315_H
#define __OLED_SSD1315_H
 
#include "stm32f4xx_hal.h"   /* 本工程为 STM32F407VET6 */
#include <stdint.h>
#include "iic.h"              /* 引入 IIC_BUS_OLED 与 iic_* API */
 
/* ==================== 配置参数 ==================== */
#define SSD1315_I2C_ADDR        0x78    /* 8位写地址: 0x3C<<1 = 0x78 (SA0=GND) */
#define SSD1315_WIDTH           128     /* 屏幕宽度 (像素) */
#define SSD1315_HEIGHT          64      /* 屏幕高度 (像素) */
#define SSD1315_PAGES           (SSD1315_HEIGHT / 8)  /* 页数 = 64/8 = 8 */
 
/* ==================== SSD1315 命令定义 ==================== */
#define SSD1315_CMD_DISPLAY_OFF         0xAE    /* 关闭显示 */
#define SSD1315_CMD_DISPLAY_ON          0xAF    /* 开启显示 */
 
#define SSD1315_CMD_SET_CONTRAST        0x81    /* 设置对比度 (后跟1字节数据) */
 
#define SSD1315_CMD_ENTIRE_DISPLAY_ON   0xA5    /* 全局点亮 (忽略RAM) */
#define SSD1315_CMD_ENTIRE_DISPLAY_OFF  0xA4    /* 正常显示 (跟随RAM) */
 
#define SSD1315_CMD_NORMAL_DISPLAY      0xA6    /* 正常显示 */
#define SSD1315_CMD_INVERT_DISPLAY      0xA7    /* 反色显示 */
 
#define SSD1315_CMD_SET_MEM_ADDR_MODE   0x20    /* 设置内存地址模式 (后跟1字节) */
#define SSD1315_ADDR_MODE_HORIZONTAL    0x00    /* 水平地址模式 */
#define SSD1315_ADDR_MODE_VERTICAL      0x01    /* 垂直地址模式 */
#define SSD1315_ADDR_MODE_PAGE          0x02    /* 页地址模式 */
 
#define SSD1315_CMD_SET_COL_ADDR        0x21    /* 设置列地址 (后跟2字节: start, end) */
#define SSD1315_CMD_SET_PAGE_ADDR       0x22    /* 设置页地址 (后跟2字节: start, end) */
 
#define SSD1315_CMD_SET_PAGE_START      0xB0    /* 设置页起始地址 (0xB0~0xB7) */
#define SSD1315_CMD_SET_COL_LOW         0x00    /* 设置低位列地址 (0x00~0x0F) */
#define SSD1315_CMD_SET_COL_HIGH        0x10    /* 设置高位列地址 (0x10~0x1F) */
 
#define SSD1315_CMD_SET_START_LINE      0x40    /* 设置显示起始行 (0x40~0x7F) */
 
#define SSD1315_CMD_SEGMENT_REMAP_0     0xA0    /* 段重映射: 列0->SEG0 */
#define SSD1315_CMD_SEGMENT_REMAP_127   0xA1    /* 段重映射: 列127->SEG0 */
 
#define SSD1315_CMD_COM_SCAN_NORMAL     0xC0    /* COM扫描方向: 从上到下 */
#define SSD1315_CMD_COM_SCAN_REMAP      0xC8    /* COM扫描方向: 从下到上 */
 
#define SSD1315_CMD_SET_MULTIPLEX       0xA8    /* 设置多路复用比 (后跟1字节) */
#define SSD1315_CMD_SET_DISPLAY_OFFSET  0xD3    /* 设置显示偏移 (后跟1字节) */
 
#define SSD1315_CMD_SET_COM_PINS        0xDA    /* 设置COM引脚配置 (后跟1字节) */
 
#define SSD1315_CMD_SET_CLOCK_DIV       0xD5    /* 设置时钟分频/振荡器频率 (后跟1字节) */
#define SSD1315_CMD_SET_PRECHARGE       0xD9    /* 设置预充电周期 (后跟1字节) */
#define SSD1315_CMD_SET_VCOMH           0xDB    /* 设置VCOMH电压 (后跟1字节) */
 
#define SSD1315_CMD_CHARGE_PUMP         0x8D    /* 电荷泵设置 (后跟1字节) */
#define SSD1315_CHARGE_PUMP_ENABLE      0x14    /* 开启电荷泵 */
#define SSD1315_CHARGE_PUMP_DISABLE     0x10    /* 关闭电荷泵 */
 
/* 滚动命令 */
#define SSD1315_CMD_SCROLL_RIGHT        0x26    /* 水平右滚动 */
#define SSD1315_CMD_SCROLL_LEFT         0x27    /* 水平左滚动 */
#define SSD1315_CMD_SCROLL_VERT_RIGHT   0x29    /* 垂直+右滚动 */
#define SSD1315_CMD_SCROLL_VERT_LEFT    0x2A    /* 垂直+左滚动 */
#define SSD1315_CMD_SCROLL_STOP         0x2E    /* 停止滚动 */
#define SSD1315_CMD_SCROLL_START        0x2F    /* 开始滚动 */
 
/* ==================== 函数声明 ==================== */
 
/* 初始化与控制 */
uint8_t OLED_Init(void);      /* 只初始化 + 清显存, 不推屏: 屏上内容由调用方首次 OLED_Update() 推送 */
void OLED_DisplayOn(void);
void OLED_DisplayOff(void);
void OLED_SetContrast(uint8_t contrast);
void OLED_SetDisplayMode(uint8_t mode);  /* 0=正常, 1=反色 */
void OLED_ClearBuffer(void);  /* 只清显存不推屏: 渲染流程用这个, 避免白推一整帧 */
void OLED_Clear(void);
void OLED_Fill(uint8_t data);
 
/* 显存操作 */
void OLED_Update(void);
void OLED_SetPixel(uint8_t x, uint8_t y, uint8_t color);
uint8_t OLED_GetPixel(uint8_t x, uint8_t y);
 
/* 字符与字符串显示 (6x8 / 8x16 字体) */
void OLED_ShowChar(uint8_t x, uint8_t y, char ch, uint8_t size);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str, uint8_t size);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t size);
void OLED_ShowHex(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t size);
void OLED_ShowFloat(uint8_t x, uint8_t y, float num, uint8_t int_len, uint8_t dec_len, uint8_t size);
 
/* 图形绘制 */
void OLED_DrawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t color);
void OLED_DrawRectangle(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t color);
void OLED_DrawFillRectangle(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t color);
void OLED_DrawCircle(uint8_t x, uint8_t y, uint8_t r, uint8_t color);
void OLED_DrawFillCircle(uint8_t x, uint8_t y, uint8_t r, uint8_t color);
 
/* 图片显示 */
void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *bmp);
 
/* 滚动效果 */
void OLED_ScrollRight(uint8_t start_page, uint8_t end_page, uint8_t speed);
void OLED_ScrollLeft(uint8_t start_page, uint8_t end_page, uint8_t speed);
void OLED_ScrollStop(void);
 
/* 光标定位 (基于字符行/列) */
void OLED_SetCursor(uint8_t row, uint8_t col);
 
#endif /* __OLED_SSD1315_H */

