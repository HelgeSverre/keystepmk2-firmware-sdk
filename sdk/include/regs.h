/* The STM32G0B1 registers the SDK uses. Addresses are from ST's reference manual RM0444. */
#ifndef KS_REGS_H
#define KS_REGS_H
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))
#define REG8(addr) (*(volatile uint8_t *)(addr))

/* core */
#define SYST_CSR REG(0xE000E010)
#define SYST_RVR REG(0xE000E014)
#define SYST_CVR REG(0xE000E018)
#define NVIC_ISER REG(0xE000E100)
#define NVIC_ICER REG(0xE000E180)
#define NVIC_ICPR REG(0xE000E280)
#define SCB_VTOR REG(0xE000ED08)
#define SCB_AIRCR REG(0xE000ED0C)

/* clocks and flash */
#define RCC 0x40021000u
#define RCC_CR REG(RCC + 0x00)
#define RCC_CFGR REG(RCC + 0x08)
#define RCC_PLLCFGR REG(RCC + 0x0C)
#define RCC_IOPENR REG(RCC + 0x34)
#define RCC_AHBENR REG(RCC + 0x38)
#define RCC_APBENR1 REG(RCC + 0x3C)
#define RCC_APBENR2 REG(RCC + 0x40)
#define FLASH_ACR REG(0x40022000)

/* GPIO ports */
#define GPIOA 0x50000000u
#define GPIOB 0x50000400u
#define GPIOC 0x50000800u
#define GPIOD 0x50000C00u
#define GPIO_MODER(p) REG((p) + 0x00)
#define GPIO_OTYPER(p) REG((p) + 0x04)
#define GPIO_OSPEEDR(p) REG((p) + 0x08)
#define GPIO_PUPDR(p) REG((p) + 0x0C)
#define GPIO_IDR(p) REG((p) + 0x10)
#define GPIO_BSRR(p) REG((p) + 0x18)
#define GPIO_AFR(p, pin) REG((p) + 0x20 + ((pin) >> 3) * 4)

/* SPI */
#define SPI1 0x40013000u
#define SPI2 0x40003800u
#define SPI3 0x40003C00u
#define SPI_CR1(s) REG((s) + 0x00)
#define SPI_CR2(s) REG((s) + 0x04)
#define SPI_SR(s) REG((s) + 0x08)
#define SPI_DR8(s) REG8((s) + 0x0C)

/* I2C */
#define I2C1 0x40005400u
#define I2C_CR1(i) REG((i) + 0x00)
#define I2C_CR2(i) REG((i) + 0x04)
#define I2C_TIMINGR(i) REG((i) + 0x10)
#define I2C_ISR(i) REG((i) + 0x18)
#define I2C_ICR(i) REG((i) + 0x1C)
#define I2C_RXDR(i) REG((i) + 0x24)
#define I2C_TXDR(i) REG((i) + 0x28)

/* USART */
#define USART1 0x40013800u
#define USART_CR1(u) REG((u) + 0x00)
#define USART_BRR(u) REG((u) + 0x0C)
#define USART_ISR(u) REG((u) + 0x1C)
#define USART_ICR(u) REG((u) + 0x20)
#define USART_RDR(u) REG((u) + 0x24)
#define USART_TDR(u) REG((u) + 0x28)

/* timers */
#define TIM6 0x40001000u
#define TIM7 0x40001400u
#define TIM15 0x40014000u
#define TIM16 0x40014400u
#define TIM17 0x40014800u
#define TIM_CR1(t) REG((t) + 0x00)
#define TIM_DIER(t) REG((t) + 0x0C)
#define TIM_SR(t) REG((t) + 0x10)
#define TIM_EGR(t) REG((t) + 0x14)
#define TIM_CCMR1(t) REG((t) + 0x18)
#define TIM_CCER(t) REG((t) + 0x20)
#define TIM_CNT(t) REG((t) + 0x24)
#define TIM_PSC(t) REG((t) + 0x28)
#define TIM_ARR(t) REG((t) + 0x2C)
#define TIM_CCR1(t) REG((t) + 0x34)
#define TIM_CCR2(t) REG((t) + 0x38)
#define TIM_BDTR(t) REG((t) + 0x44)

/* ADC and DAC */
#define ADC 0x40012400u
#define ADC_ISR REG(ADC + 0x00)
#define ADC_CR REG(ADC + 0x08)
#define ADC_CFGR2 REG(ADC + 0x10)
#define ADC_SMPR REG(ADC + 0x14)
#define ADC_CHSELR REG(ADC + 0x28)
#define ADC_DR REG(ADC + 0x40)
#define DAC 0x40007400u
#define DAC_CR REG(DAC + 0x00)
#define DAC_DHR12R1 REG(DAC + 0x08)
#define DAC_DHR12R2 REG(DAC + 0x14)
#define DAC_MCR REG(DAC + 0x3C)

/* DMA1 channel n (1-based) and its request multiplexer */
#define DMA1 0x40020000u
#define DMA_ISR REG(DMA1 + 0x00)
#define DMA_IFCR REG(DMA1 + 0x04)
#define DMA_CCR(n) REG(DMA1 + 0x08 + 0x14 * ((n) - 1))
#define DMA_CNDTR(n) REG(DMA1 + 0x0C + 0x14 * ((n) - 1))
#define DMA_CPAR(n) REG(DMA1 + 0x10 + 0x14 * ((n) - 1))
#define DMA_CMAR(n) REG(DMA1 + 0x14 + 0x14 * ((n) - 1))
#define DMAMUX_CCR(n) REG(0x40020800 + 4 * ((n) - 1))

#endif
