# Ghost Sentinel Lite — Measurements and Test Records

*Türkçe: [OLCUMLER.md](OLCUMLER.md)*

Hardware: NUCLEO-L476RG (STM32L476RG, MSI 4 MHz, Range 2) · Sound module (electret + LM393, AO/DO) ·
2× Waveshare SX1262 868M LoRa HAT (Ebyte E22-900T22S) · FreeRTOS (CMSIS-RTOS2) · Measurement: multimeter on JP6 (IDD)

---

## 1. Current measurements (MCU only, through JP6)

| Mode | Measured | Note |
|---|---|---|
| Sleep mode (CPU halted, clocks running) | ~800 µA | |
| **Stop 2, bare-metal** (EXTI wake-up) | **1–2 µA** | Matches the theoretical value in the report |
| FreeRTOS, tickless **off** | ~1 mA | Woken by SysTick every 1 ms |
| FreeRTOS, debugger connected | ~70 µA | Debug unit stays powered in Stop |
| **FreeRTOS + tickless idle (LPTIM1)** | **20–30 µA** | ~40× improvement. Gap to bare-metal is an open issue (see §6) |
| MCU awake (4 MHz) | ___ mA | to be measured |
| Sound module (with LEDs) | ___ mA | to be measured |
| LoRa HAT (WOR / Mode 0 / sleep) | ___ | to be measured |

## 2. Duty cycle — from the logs

| Condition | Wall time | Awake time | Duty cycle |
|---|---|---|---|
| Bare-metal + LPTIM heartbeat (Step 4) | 110 s | 3.7 s | 3.4 % (3.5 s of it is the main-power window) |
| Tickless, no events | 120 s | 0.325 s | **0.27 %** |
| Tickless + 25 sound events (sound analysis on) | 1040 s | 7.8 s | 0.75 % |

## 3. Tickless idle verification

Sample log (Step 5b): `[120007 ms] HEARTBEAT ... awake=325 ms | stop2=19 (timer=17 event=2) sleep=119725 ms`
→ 119.7 s of 120 s spent in Stop 2; FreeRTOS time is kept without drift by LPTIM (timestamps advance in regular 10 s steps).

## 4. LoRa tests

| Test | Result |
|---|---|
| USB-TO-LoRa-HF (DTU) ↔ SX1262 HAT (E22), HAT air rate 0.3k/1.2k/2.4k/4.8k/9.6k/19.2k/38.4k/62.5k | **No communication at any rate** (either direction): same chip, different vendor firmware |
| HAT ↔ HAT, Mode 0 | ✅ Communication works |
| HAT (WOR transmitter, REG3=0x0B) → HAT (WOR receiver, REG3=0x03), WOR period 2000 ms | ✅ Message arrives with **1–2 s** latency. REG3 bit 3: 1 = transmitter, 0 = receiver (Ebyte documentation is correct) |
| HAT → Nucleo LPUART1, wake-up from Stop 2 | ✅ (after the fixes below) |

### Problems found and fixed
1. The RX/TX labels on the HAT's Pi header are written **from the Raspberry Pi's point of view** (HAT "RX" = module output).
2. With **PCLK1** as the LPUART1 clock source, the clock stops in Stop 2 and no byte can be received → **LSE** selected.
3. Wake-up on **start bit** instead of RXNE + **WUF interrupt** enabled (the method recommended by ST).
4. `USART_ISR_BUSY` check added so the MCU does not enter Stop 2 while a byte is being received.
5. LSE (32.768 kHz) at 9600 baud gives ~3.4 clock pulses per bit → noise/framing errors; the line buffer must not be reset in the error callback. Node HAT ↔ MCU UART lowered to **4800 baud** (REG0 = `0x42`, ~6.8 pulses per bit).
6. CubeMX generated an invalid PLLSAI1 setting for the ADC clock → `Error_Handler`; ADC clock moved to SYSCLK.
7. **Bytes lost in the middle of a message** (`uya`, `uan`, `ua` instead of `uyan`): the MCU re-entered Stop 2 after every byte (~14 wake-ups per message). Diagnostic test: with Stop 2 temporarily disabled (`tl_stop_lock = 1`) there were no errors at all → the cause was the Stop 2 transitions, not the UART clock.
8. Fix: **receive window**. `HAL_UARTEx_WakeupCallback` (start-bit wake-up) and every received byte set `tl_hold_until = now + 200 ms`; during this window the tickless idle hook uses plain WFI (Sleep) instead of Stop 2. Command matching uses `strstr`.
9. Diagnostic counters (`dbg busy / hold / lock`) show why Stop 2 was skipped. They caught a forgotten test line (`lock` was incrementing every ms).

### 4.1 Reliability (HAT → Nucleo, WOR)

| Configuration | Result | `err` | Stop 2 wake-ups / message |
|---|---|---|---|
| LPUART1 on LSE, 9600 baud | Most messages corrupted | 88 / 172 bytes | – |
| LSE, 4800 baud | 7–8 / 10 woke the node, characters lost mid-message | ~5 / 73 bytes | ~14 |
| LSE, 4800 baud + 200 ms receive window | **10 commands (3× `uyan`, 7× `wake`) received intact, all switched on main power** | **0 / 88 bytes** | **2** |

- Byte check: 10 × 6 bytes (`command\r\n`) + 28-byte foreign packet = 88 = `lora_bytes` → every received message was complete.
- Idle awake ratio ≈ 0.4 % (~35 ms awake between two heartbeats); the window costs ~0.6 s of light sleep (Sleep mode) per message.
- Log (150 s): `lora=11 | awake=5245 ms | stop2=47 (timer=25 event=22) sleep=144857 ms | lora_bytes=88 err=0 | dbg busy=315 hold=7093 lock=0`
- Note: the total number of commands sent in this run was not recorded separately; what was measured is that **received messages were lossless at the UART level**. Over-the-air (WOR) loss rate needs a separate test.

### 4.2 Security observation
A packet from another device on the same channel was received: `{"target":"grp_1","poll":1}` (before the receive window it arrived corrupted as `{"targe":"gp_1","poll":}`). The firmware rejected the unknown command, but anyone sending `uyan` on the same channel could wake the sentinel → roadmap: NETID/address, E22 encryption key (REG 07–08), authenticated commands (sequence number + checksum).

## 5. Sound classification

Sampling: TIM6 → ADC1 (PA0) → DMA, 8 kHz, 1024 samples (128 ms). Features:
- **crest (peak/mean)**: max/mean of the energy in 16 × 8 ms blocks (shape in time)
- **hf %**: share of energy ≥ 1 kHz in a Goertzel sweep of 100–3000 Hz (100 Hz steps)
- Analysis time: **~1.43 M cycles = 358 ms @ 4 MHz, `-O0`** (`-O2` to be measured)

### Decision tree (final version)
```
crest ≥ 2.7 ?  → YES: hf ≥ 50 % ? → CLAP (power ON) / otherwise KNOCK (false alarm)
               → NO:  crest ≤ 2.5 AND hf ≥ 50 % ? → MOTOR (power ON) / otherwise OTHER (false alarm)
```
(The test set was recorded with the 3.5 / 2.0-without-hf thresholds; the 2.7 correction was proposed afterwards.)

### 5.1 Raw data — training set

| # | Sound | rms (mV) | peak-to-peak (mV) | crest | f_dom (Hz) | hf % |
|---|---|---|---|---|---|---|
| a1 | Clap | 2 | 61 | 5.4 | – | – |
| a2 | Clap | 7 | 77 | 6.9 | – | – |
| a3 | Clap | 3 | 37 | 2.9 | – | – |
| a4 | Clap | 15 | 227 | 6.8 | – | – |
| a5 | Clap | 7 | 83 | 4.2 | – | – |
| a6 | Clap | 15 | 137 | 3.6 | – | – |
| a7 | Clap | 5 | 60 | 4.0 | 1700 | 91 |
| a8 | Clap | 25 | 255 | 4.9 | 1900 | 91 |
| a9 | Clap | 6 | 68 | 4.1 | 1900 | 83 |
| a10 | Clap | 6 | 58 | 3.6 | 500 | 64 |
| a11 | Clap | 29 | 289 | 5.5 | 1400 | 80 |
| a12 | Clap | 5 | 48 | 3.6 | 2000 | 78 |
| a13 | Clap | 24 | 220 | 4.2 | 1800 | 87 |
| v1 | Knock | 4 | 53 | 7.3 | – | – |
| v2 | Knock | 6 | 98 | 8.3 | – | – |
| v3 | Knock | 3 | 29 | 4.7 | – | – |
| v4 | Knock | 9 | 132 | 9.5 | 100 | 10 |
| v5 | Knock | 10 | 125 | 6.5 | 400 | 22 |
| v6 | Knock | 6 | 76 | 7.6 | 100 | 6 |
| v7 | Knock | 10 | 96 | 6.6 | 100 | 16 |
| v8 | Knock | 13 | 138 | 6.1 | 100 | 9 |
| k1 | Speech | 2 | 21 | 2.3 | – | – |
| k2 | Speech | 68 | 404 | 1.7 | 600 | 6 |
| k3 | Speech | 17 | 130 | 2.3 | 300 | 2 |
| k4 | Speech | 36 | 265 | 2.4 | 1000 | 46 |
| k5 | Speech | 34 | 222 | 2.8 | 600 | 1 |
| m1 | Motor | 3 | 46 | 2.3 | 600 | 58 |
| m2 | Motor | 17 | 111 | 1.4 | 1500 | 94 |
| m3 | Motor | 20 | 137 | 1.5 | 1500 | 96 |
| m4 | Motor | 18 | 103 | 1.3 | 2000 | 98 |
| m5 | Motor | 16 | 99 | 1.4 | 1900 | 90 |
| m6 | Motor | 17 | 116 | 1.5 | 1200 | 88 |

Note: normal speech from a distance did not cross the DO threshold → the board never woke up (hardware pre-filter).

### 5.2 Raw data — test set (after the rule update)
Labels were inferred from the recording order (clap → knock → speech → motor); to be verified.

| # | Sound | rms | peak-to-peak | crest | f_dom | hf % | Decision | Power decision |
|---|---|---|---|---|---|---|---|---|
| 2 | Clap | 7 | 84 | 5.0 | 1200 | 81 | CLAP | ✅ |
| 3 | Clap | 9 | 117 | 4.4 | 1300 | 74 | CLAP | ✅ |
| 4 | Clap | 5 | 53 | 3.0 | 1200 | 77 | OTHER | ❌ missed |
| 5 | Clap | 20 | 224 | 4.9 | 1500 | 78 | CLAP | ✅ |
| 6 | Knock | 15 | 262 | 8.1 | 100 | 12 | KNOCK | ✅ |
| 7 | Knock | 4 | 36 | 4.9 | 100 | 26 | KNOCK | ✅ |
| 8 | Knock | 6 | 54 | 2.4 | 100 | 37 | OTHER | ✅ (power stayed off) |
| 9 | Speech | 25 | 194 | 2.1 | 300 | 28 | OTHER | ✅ |
| 10 | Speech | 3 | 45 | 1.9 | 2400 | 66 | MOTOR | ❌ false alarm |
| 11 | Speech | 21 | 167 | 1.8 | 600 | 5 | OTHER | ✅ |
| 12 | Speech | 19 | 152 | 2.0 | 600 | 32 | OTHER | ✅ |
| 13 | Motor | 8 | 53 | 1.5 | 1500 | 94 | MOTOR | ✅ |
| 14 | Motor | 27 | 198 | 1.2 | 2300 | 98 | MOTOR | ✅ |
| 15 | Motor | 13 | 87 | 1.4 | 1600 | 96 | MOTOR | ✅ |
| 16 | Motor | 15 | 98 | 1.7 | 1900 | 97 | MOTOR | ✅ |

### 5.3 Confusion matrices

**Training (initial rule, n=15, last full session):** 14/15 = 93 % (single error: speech → MOTOR)
**Training (updated rule, all samples with hf data):** 22/22 — optimistic, since the rule was tuned on the same data (overfitting)

**Test (updated rule, new data, n=15):**

| Actual ↓ / Decision → | CLAP | KNOCK | MOTOR | OTHER |
|---|---|---|---|---|
| Clap (4) | 3 | 0 | 0 | 1 |
| Knock (3) | 0 | 2 | 0 | 1 |
| Speech (4) | 0 | 0 | 1 | 3 |
| Motor (4) | 0 | 0 | 4 | 0 |

- 4-class accuracy: **12/15 = 80 %**
- Main-power decision accuracy: **13/15 = 87 %** (1 miss, 1 false alarm)
- Error analysis: (a) clap #4 — late wake-up, the peak was missed (crest 3.0); (b) speech #10 — very weak signal (rms 3 mV), spectrum dominated by noise.
- Proposed fix: impulse threshold 3.5 → 2.7 (to be validated with a new test set).

## 6. Open issues
- Tickless current is 20–30 µA while bare-metal Stop 2 is 1–2 µA: Test A (Step 4 bare-metal + LPTIM measurement) and Test C (heartbeat off) to be done.
- `-O0` → `-O2` analysis time comparison.
- Real load with MOSFET/relay; load-side current.
- LoRa HAT current (Mode 0 / WOR periods / sleep) and a WOR period ↔ latency ↔ current table.
- Over-the-air LoRa loss rate (sent / received, counted test) and LoRa security (encryption key).
