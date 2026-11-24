/* Vector table and reset handler. The image is linked at 0x08008000, after the factory
 * bootloader. What state the bootloader leaves behind is not known yet, so start-up assumes
 * nothing: interrupts are masked, the stack is set, and every interrupt source is switched off. */
#include <stdint.h>
#include "regs.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);

void Default_Handler(void) { for (;;) { } }
static void Null_Handler(void) { }
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));
void TIM6_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM7_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((used)) static void ks_start(void) {
    SCB_VTOR = 0x08008000;
    SYST_CSR = 0;
    NVIC_ICER = 0xFFFFFFFF;
    NVIC_ICPR = 0xFFFFFFFF;
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
    for (uint32_t *d = &_sbss; d < &_ebss;) *d++ = 0;
    main();
    for (;;) { }
}

__attribute__((naked)) void Reset_Handler(void) {
    __asm__("cpsid i\n ldr r0, =_estack\n mov sp, r0\n ldr r0, =ks_start\n bx r0\n .ltorg\n");
}

#define IRQS 32
__attribute__((section(".vectors"), used))
void (*const vectors[16 + IRQS])(void) = {
    (void (*)(void))&_estack, Reset_Handler,
    [2 ... 16 + IRQS - 1] = Default_Handler,
    [11] = Null_Handler, [14] = Null_Handler,   /* SVC, PendSV */
    [15] = SysTick_Handler,
    [16 + 8] = USB_IRQHandler,
    [16 + 17] = TIM6_IRQHandler,
    [16 + 18] = TIM7_IRQHandler,
};
