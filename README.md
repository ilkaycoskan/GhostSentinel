# Ghost Sentinel Lite

**Ultra-low-power, event-driven sentinel node on STM32L4**
Sleeps at µA level, wakes up on a sound event (clap / motor) or a LoRa wake-up packet, powers up the main system through a MOSFET, and goes back to sleep.

🇹🇷 [Türkçe README](README.tr.md)

---

## Overview

This repository is the desktop prototype of the **Sentinel Controller** of my graduation project (*Ghost Sentinel*). The idea is a **heterogeneous power architecture**:

- A small MCU (**STM32L476**) is always on duty and spends almost all of its time in **Stop 2**.
- The high-power system (in the full design an **STM32H7 flight controller**; here an LED load) is **unpowered** until a real event happens.

```
 Sound sensor ──DO──► EXTI0 ─┐
 (electret + LM393)  AO ─► ADC1+DMA (8 kHz) ─► Goertzel + envelope ─► decision tree
                             │
 LoRa HAT (WOR) ──UART──► LPUART1 (LSE, works in Stop 2)
                             │
 FreeRTOS tickless idle ◄────┘── LPTIM1 (LSE) keeps time in Stop 2
                             │
                             └──► PA8 ─► IRLZ44N ─► load ("main power")
```

## Features

| Feature | Implementation |
|---|---|
| **FreeRTOS tickless idle** | Custom `vPortSuppressTicksAndSleep()` (`configUSE_TICKLESS_IDLE = 2`) using **LPTIM1** clocked by LSE (2048 Hz), entering **Stop 2** |
| **Wake on sound** | Comparator output on **EXTI0**, chatter lock-out, then acoustic validation |
| **Sound classification** | TIM6 → ADC1 → DMA, 1024 samples @ 8 kHz; envelope crest factor + **Goertzel** scan (100–3000 Hz); decision tree: `CLAP / KNOCK / MOTOR / OTHER` |
| **Wake on LoRa** | SX1262 HAT (Ebyte E22) in **Wake-On-Radio** mode; **LPUART1 on LSE** wakes the MCU from Stop 2 on start bit |
| **Main power switching** | PA8 → IRLZ44N low-side switch; GPIO state is retained in Stop 2 |
| **Event-driven design** | ISRs only post events to a queue; a single task runs the state machine |

## Hardware

- NUCLEO-L476RG
- 2× Waveshare SX1262 868M LoRa HAT (Ebyte E22-900T22S) — one on the node, one on the PC
- Sound sensor module (electret microphone + LM393, AO/DO outputs)
- IRLZ44N, LED + 330 Ω, 220 Ω, 100 kΩ

### Wiring

| Signal | Nucleo pin | Header |
|---|---|---|
| Sound AO | PA0 (ADC1_IN5) | A0 |
| Sound DO | PB0 (EXTI0) | A3 |
| Sound VCC / GND | 3V3 / GND | CN6 |
| LoRa HAT "RX" (module output) | PC0 (LPUART1_RX) | A5 |
| LoRa HAT "TX" (module input) | PC1 (LPUART1_TX) | A4 |
| LoRa HAT 5V / GND | 5V / GND | CN6 |
| MOSFET gate (via 220 Ω, 100 kΩ to GND) | PA8 | D7 |
| Log | USART2 → ST-LINK VCP, 115200 8N1 | USB |

LPUART1: **4800 baud**, kernel clock **LSE**, wake-up on start bit.

LoRa HAT (node): UART jumper **B**, **M0 open, M1 fitted** (WOR receiver, REG3 = `0x03`), **UART 4800 baud** (REG0 = `0x42`, air rate 2.4k).
LoRa HAT (PC): UART jumper **A**, M0 open, M1 fitted (WOR transmitter, REG3 = `0x0B`), send `wake` (or `uyan`) at 9600 baud.

> Note: the RX/TX labels on the HAT's Raspberry Pi header are from the **Pi's** point of view.

## Results

### Current (MCU only, measured through JP6)

| Mode | Measured |
|---|---|
| Stop 2, bare-metal | **1–2 µA** |
| FreeRTOS, tickless disabled | ~1 mA |
| FreeRTOS + tickless + LPTIM | **20–30 µA** (~40× lower) |
| Main power ON (LED load ~9 mA) | MCU still sleeps in Stop 2 |

### Duty cycle
- 120 s without events: **0.3 s awake → 0.27 %**
- 1040 s with ~25 sound events: **0.75 %**

### LoRa wake-up
- WOR period 2000 ms → wake-up latency **1–2 s**

| Configuration | Result | UART errors | Stop 2 wake-ups / message |
|---|---|---|---|
| LPUART1 on LSE, 9600 baud | most messages corrupted | 88 / 172 bytes | – |
| LSE, 4800 baud | 7–8 / 10 woke, bytes lost mid-message | ~5 / 73 bytes | ~14 |
| LSE, 4800 baud + 200 ms receive window | **10 commands received intact, all switched on main power** | **0 / 88 bytes** | **2** |

### Sound classification (power-on decision)

| Set | Result |
|---|---|
| Training data | thresholds tuned on 32 samples |
| **Test data (new samples)** | **13/15 = 87 %** correct power decisions (1 missed clap, 1 false alarm) |

Analysis time: ~1.43 M cycles ≈ 358 ms at 4 MHz (`-O0`).

Full measurement log with raw data: [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)

## Lessons learned

1. **Same RF chip ≠ compatible radios.** Waveshare USB-TO-LoRa (DTU firmware) and the E22-based HAT never communicated across 8 air rates.
2. **LPUART clock source matters.** With PCLK1 the LPUART stops in Stop 2; it must run from **LSE** to receive and wake up.
3. **LSE at 9600 baud gives only ~3.4 clocks per bit** → frequent noise/framing errors. Lowering the node HAT ↔ MCU UART to **4800 baud** (~6.8 clocks per bit) helped; the LoRa air rate is unchanged. Also: don't discard the line buffer on a noise flag.
4. **Re-entering Stop 2 between bytes drops bytes.** The MCU went back to Stop 2 after every byte (~14 wake-ups per message) and lost characters mid-message. Disabling Stop 2 as a test removed all errors, which proved the clock was not the problem. Fix: a **200 ms receive window** started by the LPUART wake-up callback and extended by every byte; during it the idle hook uses plain WFI instead of Stop 2.
5. **SysTick race before WFI** caused spurious wake-ups; fixed by checking `PENDSTSET` after stopping SysTick.
6. **CubeMX generated an invalid PLLSAI1 config for the ADC** → `Error_Handler()`; ADC clock moved to SYSCLK.
7. **Overfitting:** the classifier scored 22/22 on the data it was tuned on, but 13/15 on new data.

## Build

1. Open the project in **STM32CubeIDE** (File → Import → Existing Projects into Workspace).
2. Build (`Debug`) and run on a NUCLEO-L476RG.
3. Open the ST-LINK virtual COM port at 115200 baud.

Key files:
- [`Core/Src/tickless_lptim.c`](Core/Src/tickless_lptim.c) — tickless idle with LPTIM1 + Stop 2
- [`Core/Src/main.c`](Core/Src/main.c) — event handling, sound classification, LoRa, power switching
- [`GhostSentinel.ioc`](GhostSentinel.ioc) — full CubeMX configuration

## Roadmap

- [ ] High-side switching for the flight controller (P-MOSFET + N-MOSFET driver + RC soft-start)
- [ ] Find the extra current in tickless mode (20–30 µA vs. 1–2 µA bare-metal)
- [ ] LoRa module current vs. WOR period vs. latency
- [ ] LoRa security: a foreign packet on the same channel was received during testing → network ID, E22 encryption key, authenticated commands
- [ ] `-O2` analysis time, fewer Goertzel bins / FFT comparison
- [ ] Integration with a PX4 flight controller over MAVLink

## License

MIT
