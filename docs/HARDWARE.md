# Hardware map (from firmware 1.0.2)

Derived 2026-11-23 from the stock image `ks2_embedded__fw1_0_2_782` only. Nothing was sent to a
device and no board was opened.

Two methods were used:

- **Reading** the Ghidra decompilation and the raw disassembly.
- **Offline emulation** of the image in Unicorn with stubbed peripherals: reset handler, HAL
  init, application init, then about 0.2 s of main loop with the TIM6, TIM7, TIM17, SysTick and
  DMA interrupts injected. Every peripheral register write was logged with its program counter.
  The external flash was modelled as a blank 512 KiB chip; all other devices return zeros.

Tags: **V** = verified, read from code or seen as a register write in the emulation trace.
**I** = inferred from context. Addresses in brackets are the functions the fact comes from.
Function names used here are listed in `ghidra/names-1.0.2.tsv`. Firmware 1.0.4 was not
cross-checked.

Alternate-function numbers are verified; the peripheral they select comes from the STM32G0B1
datasheet table as remembered, and each one matches the peripheral whose clock is enabled in the
same init routine.

## Summary

| Part | How it is driven |
|------|------------------|
| Clock | 8 MHz crystal (I), PLL to 64 MHz; HSI48 for USB |
| Display | 128×32 monochrome OLED, SSD1306 command set, SPI1 (write-only), 512-byte frame buffer |
| LEDs | 54 addressable RGB LEDs (WS2812-type protocol, GRB) in two chains of 39 and 15, TIM16_CH1 PWM + DMA on one data pin, two chain-select pins |
| Keybed | 4 select lines × 8 return lines, two contacts per key, one scan phase per 65 µs TIM7 interrupt |
| Aftertouch, pedal | ADC1 channels 1 and 0 |
| Buttons | 16 inputs on a parallel-in shift register chain read over SPI3 (receive-only) |
| Encoders | 5 quadrature encoders in a 3-select × 2-pair GPIO matrix |
| Touch strips | I2C1 device at 7-bit address 0x15, interrupt line and reset line |
| CV pitch | TIM15 CH1 + CH2, two 8-bit PWM outputs combined to 16 bits |
| CV mod 1, mod 2 | Internal DAC1 channels 1 and 2, 12 bit |
| Gate | One GPIO, two GPIOs select the gate format |
| Sync in / out | Clock on PC3 (out) and PA2 (in), run on PC0 (out) and PA3 (in); inputs on EXTI, outputs timed by TIM2 compare events |
| MIDI DIN | USART1, 31250 baud, DMA both directions |
| External flash | 4 Mbit SPI NOR on SPI2; littlefs in the top 128 KiB |
| USB | Internal full-speed device controller |

## Clock [0x08019698 `SystemClock_Config`]

| Item | Value | Tag |
|------|-------|-----|
| Flash latency | `FLASH_ACR.LATENCY = 2` | V |
| HSE | on, waits for HSERDY | V |
| HSI48 | on, waits for HSI48RDY | V |
| PLL | `RCC_PLLCFGR = 0x20001003` then PLLREN: source HSE, M = 1, N = 16, R = 2 | V |
| SYSCLK | PLL; `SystemCoreClock = 64,000,000` | V |
| HSE frequency | 8 MHz (follows from 64 MHz = HSE × 16 / 2) | I |
| AHB, APB prescalers | 1 (HPRE and PPRE cleared) | V |
| SysTick | reload 63999, 1 ms, priority 3 [0x0801c254] | V |
| USB clock | `RCC_CCIPR2 = 0`, USBSEL = HSI48 [0x0800e818] | V |
| CRS | clock never enabled (final `APBENR1 = 0x3020E033`) | V |
| I2C1, USART1 clock | `RCC_CCIPR = 0`, both on PCLK | V |

Enabled clocks after init (V, emulation trace): GPIO A, B, C, D, F; DMA1; TIM2, TIM3, TIM6,
TIM7, USB, SPI2, SPI3, I2C1, PWR, DAC1; SYSCFG, SPI1, USART1, TIM14, TIM15, TIM16, TIM17, ADC.
`SYSCFG_CFGR1 = 0x600` (both UCPD strobe bits) is written in `HAL_Init`.

No literal for the IWDG or WWDG base address exists in the image, so no watchdog is used (V for
the literal search; code could still build the address, so I for the conclusion).

## Pin table

GPIO init: plain pins and EXTI in `MX_GPIO_Init` [0x0801980c], peripheral pins in the MSP
routines named in the notes. All outputs are push-pull unless marked. Initial output levels are
written before the pins are switched to output.

| Pin | Mode | Pull | AF | Init | Function | Tag |
|-----|------|------|----|------|----------|-----|
| PA0 | analog | – | – | – | ADC1 IN0: pedal input [0x08019ccc] | V pin, I function |
| PA1 | analog | – | – | – | ADC1 IN1: aftertouch sensor [0x08019ccc, 0x08016a40] | V |
| PA2 | input, EXTI2 both edges | none | – | – | Sync in, line 2 (reads inverted) [0x080162e8] | V |
| PA3 | input, EXTI3 both edges | none | – | – | Sync in, line 1 (reads inverted), pulses time-stamped [0x080162f8, 0x0805a2e8] | V |
| PA4 | analog | – | – | – | DAC1 OUT1: CV mod [0x08019de8] | V |
| PA5 | analog | – | – | – | DAC1 OUT2: CV mod [0x08019de8] | V |
| PA6 | AF | none | 5 | – | TIM16_CH1: LED data [0x0801a69c] | V |
| PA7 | output | none | – | low | LED chain 1 select, low while that chain is sent [0x080143f4] | V |
| PA8 | output | none | – | low | External flash chip select, active low [0x08018574] | V |
| PA9 | AF open drain | up | 6 | – | I2C1 SCL [0x08019f20] | V |
| PA10 | AF open drain | up | 6 | – | I2C1 SDA [0x08019f20] | V |
| PA11, PA12 | not configured | | | | USB D−/D+ | I |
| PA13, PA14 | not configured | | | | SWD (reset state) | I |
| PA15 | input, EXTI15 rising | down | – | – | Debounced digital input, polled [0x08008e40]; purpose unknown | V |
| PB0 | output, high speed | none | – | low | LED chain 0 select, low while that chain is sent [0x080143f4] | V |
| PB1 | output | none | – | low | Gate format select A [0x080161b4] | V |
| PB2 | output | none | – | low | Keybed select bit 3 [0x080130d0] | V |
| PB3 | AF | none | 0 | – | SPI1 SCK: display [0x0801a084] | V |
| PB4 | output, medium speed | none | – | low | Button shift register load: high while shifting [0x080135b4] | V |
| PB5 | AF | none | 0 | – | SPI1 MOSI: display [0x0801a084] | V |
| PB6 | output, high speed | none | – | low | Display D/C: low = command, high = data [0x08010f70] | V |
| PB7 | output, high speed | none | – | low | Display reset, active low [0x08010f70] | V |
| PB8 | output, medium speed | up | – | low | Encoder select 0, active low [0x0800a310] | V |
| PB9 | output, medium speed | up | – | low | Encoder select 1, active low [0x0800a310] | V |
| PB10 | output | none | – | low | Keybed select bit 0 [0x080130d0] | V |
| PB11 | input at boot, then output | up | – | – | Model strap at boot (low = 37 keys), then keybed select bit 1 [0x08012e48, 0x080130d0] | V |
| PB12 | output | none | – | low | Keybed select bit 2 [0x080130d0] | V |
| PB13 | AF | none | 0 | – | SPI2 SCK: flash [0x0801a084] | V |
| PB14 | AF | none | 0 | – | SPI2 MISO: flash | V |
| PB15 | AF | none | 0 | – | SPI2 MOSI: flash | V |
| PC0 | output | none | – | high | Sync out, line 1, inverted (pin low = active) [0x080162b4] | V |
| PC1 | AF | none | 2 | – | TIM15_CH1: pitch CV, low byte [0x0801a450, 0x08015e50] | V |
| PC2 | AF | none | 2 | – | TIM15_CH2: pitch CV, high byte | V |
| PC3 | output | none | – | high | Sync out, line 2, inverted [0x080162a0] | V |
| PC4 | AF | none | 1 | – | USART1 TX: MIDI out [0x0801a7a8] | V |
| PC5 | AF | none | 1 | – | USART1 RX: MIDI in [0x0801a7a8] | V |
| PC6 | input, EXTI6 falling | none | – | – | Touch controller interrupt; flag polled, no IRQ [0x080093c4] | V |
| PC7 | output | none | – | low | Touch controller reset, active low [0x0800ade4] | V |
| PC8 | output | none | – | low | Gate format select B [0x080161b4] | V |
| PC9 | output | none | – | low | Gate output [0x0801620c] | V |
| PC10 | AF | none | 4 | – | SPI3 SCK: button shift register [0x0801a084] | V |
| PC11 | AF | none | 4 | – | SPI3 MISO: button shift register | V |
| PC12 | input | up | – | – | Encoder pair 1, phase A [0x0800a310] | V |
| PC13 | input | up | – | – | Encoder pair 1, phase B | V |
| PC14 | input | up | – | – | Encoder pair 2, phase A | V |
| PC15 | input | up | – | – | Encoder pair 2, phase B | V |
| PD0–PD6 | input | none | – | – | Keybed return bits 0–6, active low [0x080130d0] | V |
| PD8 | input | none | – | – | Keybed return bit 7, active low | V |
| PD9 | output | none | – | low | Encoder select 2, active low [0x0800a310] | V |
| PF0, PF1 | not configured | | | | HSE crystal (port F clock is enabled) | I |

Every other pin is left in its reset state.

Board code addresses pins by a 16-bit id, `port × 16 + pin` (port A = 0), through
`pin_write` [0x080157d0] and `pin_read` [0x080157f8]; the port table is at 0x0806a4c4. Example:
0x27 = PC7, 0x39 = PD9.

## Peripherals

| Peripheral | Base | Configuration | Use | From |
|------------|------|---------------|-----|------|
| SPI1 | 0x40013000 | master, CPOL 0 CPHA 0, PCLK/8 = 8 MHz, 8 bit, software NSS; `CR1 = 0x314` | Display, transmit only (no MISO pin) | 0x08019f90 |
| SPI2 | 0x40003800 | master, mode 0, PCLK/4 = 16 MHz, 8 bit; `CR1 = 0x30C` | External flash | 0x08019fe0 |
| SPI3 | 0x40003C00 | master, mode 0, PCLK/64 = 1 MHz, receive-only; `CR1 = 0x72C` | Button shift register | 0x0801a030 |
| I2C1 | 0x40005400 | `TIMINGR = 0x10303AFF` (about 100 kHz at 64 MHz), 7-bit addressing | Touch controller | 0x08019eac |
| USART1 | 0x40013800 | `BRR = 0x800` (31250 baud), 8N1, TX + RX, DMA both ways | MIDI DIN | 0x0801a7a8 |
| ADC1 | 0x40012400 | 12 bit, clock PCLK/2, sampling 160.5 cycles, channels 0 and 1, one-shot DMA of 2 samples restarted by software | Pedal, aftertouch | 0x08019c30 |
| DAC1 | 0x40007400 | both channels, `MCR` mode 2 (pin output, buffer off), no trigger, self-calibrated at start | CV mod 1 and 2 | 0x08019d68 |
| TIM2 | 0x40000000 | 32 bit, `SMCR = 0x27` (external clock mode 1 from ITR2 = TIM3 TRGO), 4 compare channels in timing mode with interrupts | Musical tick counter and event scheduler | 0x0801a160 |
| TIM3 | 0x40000400 | PSC 0, ARR variable, `CR2.MMS` = update, update interrupt | Tempo divider feeding TIM2 | 0x0801a284 |
| TIM6 | 0x40001000 | PSC 63, ARR 332: 333 µs | Soft-timer tick for the task scheduler | 0x0801a310 |
| TIM7 | 0x40001400 | PSC 63, ARR 64: 65 µs | Keybed scan | 0x0801a390 |
| TIM14 | 0x40002000 | PSC 0, ARR 57; time base only | Not started in any path seen | 0x0801a40c |
| TIM15 | 0x40014000 | PSC 3, ARR 254 (62.7 kHz), CH1 + CH2 PWM mode 1 with preload | Pitch CV | 0x0801a450 |
| TIM16 | 0x40014400 | PSC 0, ARR 79 (800 kHz), CH1 PWM mode 1 with preload, compare values by DMA | LED data | 0x0801a6ec |
| TIM17 | 0x40014800 | PSC 63, ARR 0xFFFF: 1 MHz free-running, update interrupt | Microsecond clock | 0x0801a5b8 |
| USB | 0x40005C00 | device, `BCDR.DPPU` set, `PWR_CR2.USV` set | USB MIDI, audio control, DFU hand-off | 0x0800e818 |
| EXTI | 0x40021800 | lines 2, 3 (port A, both edges), 6 (port C, falling), 15 (port A, rising) unmasked | Sync in, touch interrupt | 0x0801980c |

### DMA [0x08019e54, MSP routines]

| DMA1 channel | DMAMUX request | `CCR` | Use | From |
|--------------|----------------|-------|-----|------|
| 1 | 5 (ADC1) | 0x580: 16 bit, memory increment, one-shot | ADC samples to 0x20019de4 (2 words) | 0x08019ccc |
| 2 | 50 (USART1_RX) | 0xA0: circular, memory increment | MIDI in, 256-byte ring at 0x2001838c | 0x0801a7a8, 0x0800e360 |
| 3 | 51 (USART1_TX) | 0x90: memory to peripheral | MIDI out, buffer at 0x20018284 | 0x0801a7a8, 0x0800e484 |
| 4 | 44 (TIM16_CH1) | 0x590: memory to peripheral, 16 bit | LED compare values, 1113 words from 0x200192e8 to `TIM16_CCR1` | 0x0801a62c |

DMA2 is not used.

### Interrupts

The vector table at 0x08008000 has 16 system entries and 31 IRQ entries (the word at index 47
is already code). Unlisted entries point at the default handler 0x08021f14 (infinite loop).

| IRQ | Vector | Handler | Priority | Does | Tag |
|-----|--------|---------|----------|------|-----|
| – | NMI | 0x0801aa3c | – | infinite loop | V |
| – | HardFault | 0x0801aa40 | – | reports "hardfault", infinite loop | V |
| – | SVC, PendSV | 0x0801aa50, 0x0801aa54 | – | empty | V |
| – | SysTick | 0x0801aa58 | 3 | HAL tick + 1 | V |
| 6 | EXTI2_3 | 0x0801aa60 | 1 | PA2 and PA3 edges, rising and falling separately, to the sync-in callbacks | V |
| 8 | USB | 0x0801aab0 | 0 | `HAL_PCD_IRQHandler(0x20018770)` | V |
| 9 | DMA1 channel 1 | 0x0801aac0 | 0 | ADC DMA complete | V |
| 10 | DMA1 channels 2–3 | 0x0801aad0 | 0 | USART1 DMA [0x0800e5f0] | V |
| 11 | DMA1 channels 4–7, DMA2 | 0x0801aad8 | 0 | LED DMA complete (handle 0x2001a5c8) | V |
| 15 | TIM2 | 0x0801aae8 | 1 | CC1–CC4 to four application callbacks with the captured compare value | V |
| 16 | TIM3_TIM4 | 0x0801ab64 | 1 | TIM3 update: tempo tick callback | V |
| 17 | TIM6_DAC_LPTIM1 | 0x0801ab84 | 1 | TIM6 update: scheduler soft timers [0x08009b54] | V |
| 18 | TIM7_LPTIM2 | 0x0801aba4 | 1 | Keybed scan step [0x080130d0] | V |
| 22 | TIM17 | 0x0801abc4 | 1 | Adds 0x10000 to the microsecond high word at 0x20017814 | V |
| 27 | USART1 | 0x0801abe4 | 1 | empty (enabled, does nothing) | V |

EXTI4_15 is not enabled in the NVIC: lines 6 and 15 only latch pending flags.

## Display [0x08010f70, 0x0800fa7c, 0x0800fadc]

- Bus: SPI1, 8 MHz, write-only. PB6 = D/C, PB7 = reset. No chip-select pin is driven (V).
- Reset: PB7 high, low, 1 ms, high; then D/C low and 28 command bytes from 0x08068724 (V):

  `AE D5 80 A8 1F D3 00 40 8D 14 20 00 A1 22 00 03 C8 DA 02 81 7F D9 F1 DB 40 A4 A6 AF`

- This is the SSD1306 command set: multiplex 0x1F (32 rows), charge pump on (`8D 14`),
  horizontal addressing (`20 00`), pages 0–3 (`22 00 03`), segment remap and reversed COM scan,
  COM pins `DA 02`. Controller type SSD1306 or a compatible is I; resolution 128×32 is I from
  multiplex 32, four pages and the 512-byte frame.
- Frame buffer: 0x20018cfc, 512 bytes, one bit per pixel, page order (V). A refresh sends all
  512 bytes with D/C high and no address commands; the controller wraps by itself (V).
- Contrast: command `81 nn` [0x0800fadc] (V).

## LEDs [0x080145fc, 0x0801441c, 0x080143f4, 0x0801469c]

- One data line, PA6 = TIM16_CH1, 800 kHz PWM. Compare values come by DMA: 38 of 80 counts
  (594 ns high) for a 1 bit, 19 counts (297 ns) for a 0 bit (V). This is the WS2812/SK6812
  family protocol; the LED part itself is not identified.
- Colours are stored as 32-bit words (default `0xFF000000`); bytes 1, 2, 0 of each word are sent
  in that order, MSB first, which is green, red, blue if the word is `0x00RRGGBB` (V order, I
  naming).
- Two chains share the data line: chain 0 with 39 LEDs (buffer 0x20019160) and chain 1 with 15
  LEDs (buffer near 0x2001910c). A select pin goes low for the duration of one chain's transfer
  and high after it: PB0 for chain 0, PA7 for chain 1 (V). External gating logic is I.
- Each transfer is 1113 words: one 19-count slot, 175 zero slots (219 µs low, the reset gap),
  then up to 39 × 24 bit slots, then a zero (V).
- Chain 0 was resent about every 6 ms in the emulated idle loop; chain 1 only on change (V in
  emulation).
- Total 54 LEDs (V). Which index is which LED on the panel is not determined.

## Keybed [0x08012e48, 0x080130d0, 0x08012ecc]

- Model strap: at start-up PB11 is driven low, switched to input with pull-up, and read after
  1 ms. Low gives 10 scan phases and 37 keys (KeyStep 37 mk2); high gives 8 phases and 32 keys
  (V).
- Matrix: select lines PB10, PB11, PB12 (address bits 0–2) and PB2; return lines PD0–PD6 and PD8,
  read inverted as one byte (V). External decoder is I.
- One phase per TIM7 interrupt (65 µs). Phases 0–7 put the phase number on PB10–PB12 with PB2
  low; phases 8 and 9 use addresses 6 and 7 with PB2 high (V). A full scan takes 520 µs (32
  keys) or 650 µs (37 keys).
- Phases come in pairs: the even phase reads the first contact of 8 keys, the odd phase the
  second contact. Pair `g` covers keys `base .. base+7` with `base = {0, 8, 16, 24, 32}[n/2 − 1 − g]`
  (table at 0x0806a304), bit 7 = lowest key of the group (V). Keys above 36 are ignored.
- Velocity: the microsecond time (TIM17) is stored when the first contact closes; when the
  second closes, `(t2 − t1) × 10 / divisor` is queued with the key number (bit 7 set = note on).
  Release works the same way in reverse (V). Events go to an 8-byte-per-entry ring.
- Aftertouch: a single analog value on ADC1 channel 1 (PA1), not per key. The min/max
  calibration [0x08016a40, 0x08016a90] reads sample index 1 of the ADC buffer (V); files
  `calib/aft_factory` and `calib/aft_user`.

## Buttons [0x080135b4]

- PB4 high, `HAL_SPI_Receive(SPI3, 2 bytes)`, PB4 low: a 16-bit parallel-in shift register chain
  (two 8-bit parts is I). PB4 low loads, high shifts (V).
- The two bytes form a little-endian word. Bits 0–7 and 9–15 are each debounced and call their
  own handler; the mapping differs between the 32- and 37-key models (flag at object + 0xE8) (V).
  Which bit is which button is not determined.
- Scanned from the main loop: about 1,100 reads in 195 ms of emulated time.

## Encoders [0x0800a310, 0x0800ade4]

Five quadrature encoders in a matrix. A select pin is driven low, both phases are read, and the
select pin is driven high again (V, emulation trace and object fields).

| Encoder object | Select | Phase A | Phase B |
|----------------|--------|---------|---------|
| 0x200176e0 | PB8 | PC12 | PC13 |
| 0x2001768c | PB9 | PC12 | PC13 |
| 0x20017638 | PD9 | PC12 | PC13 |
| 0x200175e4 | PB9 | PC14 | PC15 |
| 0x20017590 | PD9 | PC14 | PC15 |

The object addresses are from the 37-key code path (the emulated strap read low). Which encoder
is which knob is not determined.

## Touch controller [0x0800ade4, 0x080093c4, 0x080093e0, 0x08009430]

- I2C1, 8-bit address 0x2A/0x2B, so 7-bit 0x15 (V).
- Start-up: PC7 low 50 ms, high, 50 ms, write `64 28 05`, 50 ms, read 4 bytes from register
  0x04 (V).
- Run time: a falling edge on PC6 sets EXTI pending bit 6; a task polls the flag and then reads
  4 bytes without a register address (V). The 4 bytes are handled as two channels of two bytes
  each [0x08009430] (V).
- Two channels fit the pitch and mod touch strips (I). The chip is not identified.

## CV, gate and sync [0x08015c4c and neighbours]

- **Pitch:** `cv_pitch_set` [0x08015e50] converts the pitch (linear, or exponential through the
  2^(n/12) table at 0x0806a4ec; volt-per-octave and hertz-per-volt, I) through a calibration curve to a 16-bit value and writes the
  low byte to `TIM15_CCR1` (PC1) and the high byte to `TIM15_CCR2` (PC2). Two 8-bit PWM signals
  at 62.7 kHz are therefore summed outside the MCU, coarse and fine (V for the registers, I for
  the summing). Calibration file `calib/pitch`.
- **Mod 1, mod 2:** `cv_mod_set` [0x08016068] writes a calibrated value to the DAC with 12-bit
  left alignment (`DHR12L1` / `DHR12L2`), channels 1 and 2 on PA4 and PA5 (V). The first CV
  object uses channel 1 and the second channel 2 [0x08015e34]; that these are Mod 1 and Mod 2 in
  that order is I. Maximum voltage is a scale factor [0x08016048]. Files `calib/mod1`,
  `calib/mod2`.
- **Gate:** PC9 [0x0801620c]. With gate format 0 the pin is low when the gate is on; with any
  other format it is high when on (V).
- **Gate format:** PB1 and PC8 [0x080161b4] (V). Format 0: both low. Format 1: PB1 low, PC8
  high (also the power-on state). Format 2: PB1 high, PC8 low. Which menu entry (S-trig, V-trig
  5 V, the third option) is which number, and what the pins switch, is not determined.
- **Sync out:** PC0 and PC3, both inverted in software (argument 1 drives the pin low)
  [0x080162a0, 0x080162b4] (V).
- **Sync in:** PA2 and PA3, both read inverted, both edges on EXTI [0x0801aa60] (V). PA3 rising
  events are time-stamped into a ring [0x0805a2e8], which fits a clock input (I).
- A built-in loopback test [0x0801630c] drives PC3 and expects it on PA2, and PC0 on PA3 (V).
- **Which line is which** (V in emulation, see "CV, sync and MIDI in, from emulation"): PC3 and
  PA2 are the clock, PC0 and PA3 the run line.
- **Timing:** TIM3 divides 64 MHz to the musical tick and clocks TIM2; the four TIM2 compare
  channels raise interrupts for scheduled events (V). Tempo: `TIM3_ARR = 1,536,000,000 /
  (tempo × 480 / 50) − 1` [0x08018db4] (V); the unit of `tempo` is not determined.

## CV, sync and MIDI in, from emulation

Run on 2026-11-23 with stock 1.0.2 in `tools/emu.py`, a blank external flash (so no calibration)
and default settings. Register values only: nothing here is a voltage.

- **Pitch:** the 16-bit value is `(note − 36) × 65536 / 120`: 546 counts per semitone, ten
  octaves over the full range, zero at MIDI note 36. Key 0 (note 53) gives 9284, note 60 gives
  13106. A one-volt-per-octave output over 10 V fits this (I).
- **Mod 1** follows the velocity of the last note (1168 to 1217 for velocities 96 to 100);
  **Mod 2** follows the Mod strip and CC 1 (1545 at the top). Both are the default assignments.
- **Gate:** PC9 goes high about 8 ms after a key goes down and low when the last key is
  released; a second key played legato keeps it high.
- **Sync out while playing:** PC3 pulses once per sixteenth (60 ms on, 65 ms off at 120 BPM);
  PC0 is active for as long as the sequencer runs.
- **Sync in:** pulses on PA2 are taken as a clock: with a pulse every 50 ms the MIDI clock
  output went from 48 to about 115 per second, six per pulse. Each pulse on PA3 made the
  firmware send Start and Stop, so PA3 is the run line.
- **MIDI in on DIN:** a note on channel 1 drives pitch and gate and is not echoed. CC 1 and
  pitch bend are passed to both outputs and move Mod 2 and the pitch (half the bend range is
  one semitone). Clock, Start and Stop are followed and forwarded. SysEx on the DIN input gets
  no reply: the Arturia requests are answered on USB only.
- **Sequencer timing:** TIM3 overflows clock TIM2 (external clock mode 1); the firmware
  schedules its events as TIM2 compare values. The emulator models that chain, and the stock
  sequencer then sends MIDI clock every 20.83 ms at 120 BPM.

## The 37-key model, from emulation

With PB11 strapped low the same image runs as the KeyStep 37 mk2 (V in emulation):

- USB product id `0x0229`, product string "KeyStep 37 mk2".
- 37 keys; key 0 sends note 48 and key 36 note 84.
- Four more encoders in the matrix send CC 16, 17, 18 and 19 (select PB9 and PD9, on both
  phase pairs), with acceleration.
- Button bit 7 opens a help screen ("Functions to Control: Set Arp Chord Mode Rec"), bit 15
  shows the four CC numbers and values and lights LED 13 of chain 1. Bit 8 did nothing.

## Fonts [0x08068740, 0x08069104]

The image carries two raster fonts in the Windows `.FNT` version 2 format, characters 0x20 to
0xFF (V: parsed by `tools/fontextract.py`, and text drawn from them matches the stock screens):

| Font | Address in 1.0.2 | Height | Widths | Copyright field |
|---|---|---|---|---|
| Small | 0x08068740 | 9 px | up to 6 px | `copyright ADAPTIVE 2002` |
| Large | 0x08069104 | 15 px, ascent 12 | up to 15 px | `atn.` |

1.0.4 has the same two fonts at 0x08069ADC and 0x0806A4A0.

## MIDI DIN [0x0801a7a8, 0x0800e360, 0x0800e484]

USART1 on PC4 (TX) and PC5 (RX), 31250 baud, 8N1 (V). Receive runs by circular DMA into a
256-byte ring; transmit by one-shot DMA with a transfer-complete interrupt (V).

## Pedal

ADC1 channel 0 on PA0 is the only analog input besides aftertouch, and the menu has one "Pedal
Input" with Expression, Sustain and AutoDetect, so the pedal jack is read by the ADC (I).
Calibration file `calib/expr_pdl`.

## External flash [0x08018574, 0x08017f08, 0x0801788c, 0x080188a0]

- Bus: SPI2 at 16 MHz, chip select PA8 (V).
- Accepted JEDEC ids (command 0x9F): `1F 84 01` and `C8 40 13`. Anything else logs "Wrong driver
  / chip reference for the external flash" and continues (V). Both ids are 4 Mbit parts; the
  names Adesto AT25SF041 and GigaDevice GD25Q40 are from memory (I).
- Size: every access checks `address + length ≤ 0x80000`, so 512 KiB (V).
- Commands used (V): `66`, `99` (reset), `9F` (id), `03` + 3 address bytes (read), `06` (write
  enable), `02` (page program, 256 bytes at most), `20` (4 KiB erase), `05` (status; busy bit
  polled for up to 300 ms).
- littlefs: block size 0x1000, 32 blocks, read/program/cache size 0x80, lookahead 0x20, block
  cycles 50000, name limit 0x20. Block `n` is at flash offset `(n + 0x60) × 0x1000`, so the file
  system occupies 0x60000–0x7FFFF (V) [0x08018194, 0x08018bbc].
- 0x00000–0x5FFFF is addressed raw, through two names the file layer treats specially
  [open 0x08018484, read 0x0801824c, write 0x08018ae8, delete 0x080189ec] (V):

  | Range | Name | Contents |
  |---|---|---|
  | 0x00000–0x3FFFF | `presets/N`, N = 0–63 | One pattern per 4 KiB block `N`; a write at offset 0 erases the block first. The block is the 4096-byte pattern blob of a `.keystep2` file, with N = (bank − 1) × 16 + (pattern − 1) (V in emulation: five patterns loaded into the flash image read back identical over SysEx) |
  | 0x40000–0x40FFF | `settings` | Global settings; 0x68 bytes are read at start-up (V in emulation). Layout below |
  | 0x41000–0x5FFFF | none | No name maps to blocks 0x41–0x5F: 124 KiB the stock firmware never addresses. The SDK stores its data here. Blank on the one unit read (2026-11-25). See "The SDK's area across versions" below |

  Every other name goes to littlefs (`calib/aft_factory`, `calib/aft_user`, `calib/pitch`,
  `calib/mod1`, `calib/mod2`, `calib/expr_pdl`). The factory calibration therefore lives in
  0x60000–0x7FFFF and cannot be rebuilt from the firmware package.
- On a blank chip the firmware formats the file system at the first start and creates a `calib`
  directory and a `boot_count` file: three sectors from 0x60000 are written (V in emulation).
- **The SDK's area across versions** (checked 2026-11-25):
  - 1.0.4: the eleven functions of the file layer and flash driver (name to block
    [0x08018484], raw read, write and delete, the littlefs block read and program, the driver)
    are identical to 1.0.2's after normalising addresses, 0x4C higher up (V). The stored names
    are the same in 1.0.2, 1.0.3 and 1.0.4: `presets/`, `settings`, `calib/…`, `boot_count`.
  - 1.0.2, 1.0.3 and 1.0.4 run in the emulator on a unit's own chip image, with every sector
    read, programmed or erased logged: start-up reads 0x00, 0x40 and file-system sectors and
    programs 0x60; saving a pattern erases and programs its sector; saving the settings erases
    and programs 0x40; nothing in 0x41–0x5F is touched by any of the three (V in emulation).
  - `All Patterns Init` (answered Yes) erases sectors 0x00–0x3F and nothing else, in 1.0.2 and
    1.0.4 (V in emulation). 1.0.3 was not run through it.
  - Flashing a firmware does not touch this chip: the external flash read on a unit after
    several updates still held its patterns and settings.
- **Settings block** (V in emulation: written after SysEx operation `19 92`, then compared with
  the values the read operation `2A 89` returns). 104 bytes:

  | Offset | Size | Content |
  |---|---|---|
  | 0 | 8 | Header: `FF FF 02 41 B5 D1 00 00` in the one sample. Meaning not determined (a version or check value, I) |
  | 8 | 10 | Group 1 (ids `0x2000`+), 5 little-endian words |
  | 18 | 30 | Group 2 (`0x4000`+), 15 words |
  | 48 | 24 | Group 3 (`0x6000`+), 12 words in 1.0.2 |
  | 72 | 4 | Groups 4 and 5, one word each (the bit-masked groups) |
  | 76 | 14 | Group 6 (`0xC000`+), 7 words |
  | 90 | 12 | Group 7 (`0xE000`+), 6 words |

  A setting written with `36 FB` changes RAM only; nothing reached the flash until `19 92` was
  sent. 1.0.4 has 12 more words in group 3, so its block is longer (I).

## USB [0x0800e818]

Internal device controller at 0x40005C00, clocked from HSI48 without CRS trimming being enabled.
D+ pull-up through `BCDR.DPPU`. HAL PCD handle at 0x20018770. No VBUS sensing pin was found.

Seen in the emulator after start-up (V in emulation, 2026-11-23):

- `RCC_CR` has HSI48 on, `RCC_CCIPR2` is 0 (USB clocked from HSI48), the CRS is never clocked.
- `PWR_CR2 = 0x400` (USV: USB supply valid).
- `SYSCFG_CFGR1 = 0x600`: both USB-C strobe bits, which switch off the dead-battery pull-downs
  on PA8, PB15, PD0 and PD2. Those pins are the flash chip select, the flash data line and two
  keybed returns, so firmware that skips this gets pull-downs on them.
- `USB_CNTR = 0xBF80`; the interrupt is number 8, shared, and the handler first tests bit 2 of
  `SYSCFG_ITLINE8` (0x400100A0).
- Packet memory: endpoint 0 sends from 0x80 and receives at 0x40; endpoint 1 (IN) sends from
  0xC0; endpoint 2 (OUT) receives at 0x180. All 64 bytes.
- With the host model in `tools/usbmodel.py` the firmware enumerates as `1C75:0228`,
  "Arturia" / "KeyStep mk2" / serial "0123456789ABCDEF", and answers the SysEx version request
  on its USB MIDI port with `1.0.2.782`, the same reply a real unit gave. The serial is the
  image's default; whether a real unit reports another one is not known.

## Memory and run-time [0x08021ec4, 0x08021e60, 0x08061aac]

| Region | Range | Size | Tag |
|--------|-------|------|-----|
| Vector table / image start | 0x08008000 (written to VTOR) | | V |
| `.data` load image | 0x0807047c | 0x8CC | V |
| `.data` | 0x20000020–0x200008ec | 2.2 KiB | V |
| `.bss` | 0x200008f0–0x2001b0f0 | 106 KiB | V |
| Heap (`_sbrk`) | 0x2001b0f0 upward, limit 0x20023c00 | 34.8 KiB at most | V |
| Stack | top 0x20024000, 1 KiB reserved by `_sbrk`; about 1.4 KiB used after init in emulation | | V |

- No RTOS: SVC and PendSV are empty, SysTick only counts. `main` runs init and then calls the
  `run` method of two scheduler objects in an endless loop [0x08061aac] (V). Periodic work is
  done by soft timers ticked from TIM6 and by polling tasks.
- Time bases: HAL millisecond tick at 0x2001a700; microseconds = `TIM17_CNT` + high word at
  0x20017814 [0x0800b964] (V).
- Interrupt handlers reach the application through function pointers at 0x200000e4–0x20000100
  (V).

## Control map from emulation

Found on 2026-11-23 by running stock 1.0.2 in `tools/emu.py`, driving one input at a time and
reading the display, the LED data and the MIDI sent on USART1. Nothing here has been checked on
a device. It supersedes the "not determined" notes above for the 32-key model.

**Buttons** (shift-register bit, 1 = pressed):

| Bit | Button | Bit | Button |
|-----|--------|-----|--------|
| 0 | Mutate | 9 | Encoder push |
| 1 | Arp | 10 | Back |
| 2 | Seq | 11 | Play |
| 3 | Shift | 12 | Stop |
| 4 | Hold | 13 | Record |
| 5 | Oct + | 14 | Chord |
| 6 | Oct − | 7, 8, 15 | No reaction on the 32-key model |

With all 16 bits high the firmware never draws its start-up screen, so released is 0.

**Tempo** is not on the shift register. It is PA15: pulled down, high when pressed. That is the
pin the pin table lists as "purpose unknown", and it fits Arturia's instruction to hold Tempo at
power-on for update mode: the bootloader can read one pin without the SPI chain (inferred).

**Encoder:** one encoder on the 32-key model, phases on PC12/PC13, idle high. The firmware accepts
a quarter step every 12 ms; 2 ms was too fast for its debounce. The PC14/PC15 pair had no effect.

**Touch controller:** each report is two little-endian 16-bit words; the low byte is the finger
position, 0 for no finger. Word 0 is the Mod strip (sends CC 1), word 1 the Bend strip (pitch
bend). About 22 or less is the top (CC 127, bend +8191) and about 160 the bottom (CC 0, bend
−8192), linear in between. Bend returns to centre in steps when released; Mod keeps its value.

**Keys:** key 0 sends note 53 (F3) and key 31 note 84 (C6) at octave 0. With 6 ms between the two
contacts the velocity is 100.

**Button LEDs** (chain 1, 15 LEDs):

| Index | Button | Index | Button |
|-------|--------|-------|--------|
| 0 | Oct − (inferred) | 7 | Chord |
| 1 | Oct + | 8 | Tempo (inferred) |
| 2 | Hold | 9 | Record |
| 3 | Shift | 10 | Stop |
| 4 | Seq | 11 | Play |
| 5 | Arp | 12 | Encoder |
| 6 | Mutate (inferred) | 13, 14 | Never lit |

Indices 0 to 12 run a colour sweep at start-up. Chain 0 (39 LEDs) stayed dark in every state
tried, so its layout is still unknown.

**Screens seen:** version number at start-up, then "Arturia KeyStep" when neither Seq nor Arp is
on; "Tempo / Bank / Preset" in Seq mode; "Arp | Up" in Arp mode; the Global Settings menu.

## Not determined

- Exact MCU part number, package and flash size; option bytes; the bootloader.
- The OLED controller part (only its command set), and whether its chip select is tied low.
- The LED part, and the order of the 54 LEDs on the panel.
- The touch controller chip at I2C address 0x15, and the meaning of `64 28 05` and of the
  4-byte report.
- Which shift register bit is which button; which encoder object is which knob; where the
  encoder push switches are read.
- The purpose of PA15 (input, pull-down, EXTI 15 rising, polled).
- Which of PA0 and the pedal jack contacts does what; how sustain and expression are told apart.
- The jack wiring and the analog stages behind the sync, gate, pitch and mod pins (voltage
  ranges, filters, gate format switching).
- The unit of the tempo value and the tick resolution of TIM2.
- The purpose of TIM14.
- Whether 0x41000–0x5FFFF is blank on a real unit; the settings block's 8-byte header; the
  contents of the calibration files.
- Keybed velocity divisor and curve; key numbering direction on the 32-key model.
- Pins not configured by the firmware (PA11–PA14, PD7, PD10 and up, ports E and F): assumed
  unused or fixed-function.
- Whether 1.0.4 changes any of this.
