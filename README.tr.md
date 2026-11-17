# Ghost Sentinel Lite

**STM32L4 üzerinde ultra düşük güçlü, olay tetiklemeli nöbetçi düğüm**
µA seviyesinde uyur; ses (alkış / motor) ya da LoRa uyandırma paketiyle uyanır, MOSFET üzerinden ana sistemin gücünü açar ve tekrar uykuya döner.

🇬🇧 [English README](README.md)

---

## Genel bakış

Bu repo, bitirme projemin (*Ghost Sentinel*) **Nöbetçi Kontrolcü** kısmının masaüstü prototipidir. Fikir bir **heterojen güç mimarisi**:

- Küçük bir MCU (**STM32L476**) sürekli nöbette ve zamanının neredeyse tamamını **Stop 2**'de geçiriyor.
- Yüksek güçlü sistem (tam tasarımda bir **STM32H7 uçuş kontrolcüsü**, burada bir LED yük) gerçek bir olay olana kadar **enerjisiz** kalıyor.

```
 Ses sensörü ──DO──► EXTI0 ─┐
 (elektret + LM393)  AO ─► ADC1+DMA (8 kHz) ─► Goertzel + zarf ─► karar ağacı
                            │
 LoRa HAT (WOR) ──UART──► LPUART1 (LSE, Stop 2'de çalışır)
                            │
 FreeRTOS tickless idle ◄───┘── LPTIM1 (LSE) Stop 2'de zamanı tutar
                            │
                            └──► PA8 ─► IRLZ44N ─► yük ("ana güç")
```

## Özellikler

| Özellik | Uygulama |
|---|---|
| **FreeRTOS tickless idle** | Kendi yazdığım `vPortSuppressTicksAndSleep()` (`configUSE_TICKLESS_IDLE = 2`): LSE ile beslenen **LPTIM1** (2048 Hz), **Stop 2** |
| **Sesle uyanma** | Komparatör çıkışı **EXTI0**'a, titreşim kilidi, ardından akustik doğrulama |
| **Ses sınıflandırma** | TIM6 → ADC1 → DMA, 8 kHz'te 1024 örnek; zarf tepe/ortalama oranı + **Goertzel** taraması (100–3000 Hz); karar ağacı: `ALKIŞ / VURMA / MOTOR / DİĞER` |
| **LoRa ile uyanma** | SX1262 HAT (Ebyte E22) **Wake-On-Radio** modunda; **LSE ile beslenen LPUART1** start bitinde MCU'yu Stop 2'den uyandırır |
| **Ana güç anahtarlama** | PA8 → IRLZ44N alçak taraf anahtar; GPIO durumu Stop 2'de korunur |
| **Olay tabanlı tasarım** | Kesmeler sadece kuyruğa olay atar; tek bir görev durum makinesini işletir |

## Donanım

- NUCLEO-L476RG
- 2× Waveshare SX1262 868M LoRa HAT (Ebyte E22-900T22S): biri düğümde, biri PC'de
- Ses sensörü modülü (elektret mikrofon + LM393, AO/DO çıkışları)
- IRLZ44N, LED + 330 Ω, 220 Ω, 100 kΩ

### Bağlantılar

| Sinyal | Nucleo pini | Başlık |
|---|---|---|
| Ses AO | PA0 (ADC1_IN5) | A0 |
| Ses DO | PB0 (EXTI0) | A3 |
| Ses VCC / GND | 3V3 / GND | CN6 |
| LoRa HAT "RX" (modülün çıkışı) | PC0 (LPUART1_RX) | A5 |
| LoRa HAT "TX" (modülün girişi) | PC1 (LPUART1_TX) | A4 |
| LoRa HAT 5V / GND | 5V / GND | CN6 |
| MOSFET kapısı (220 Ω seri, 100 kΩ GND'ye) | PA8 | D7 |
| Log | USART2 → ST-LINK VCP, 115200 8N1 | USB |

LPUART1: **4800 baud**, saat kaynağı **LSE**, start bitinde uyanma.

LoRa HAT (düğüm): UART jumper **B**, **M0 açık, M1 takılı** (WOR alıcı, REG3 = `0x03`), **UART 4800 baud** (REG0 = `0x42`, hava hızı 2.4k).
LoRa HAT (PC): UART jumper **A**, M0 açık, M1 takılı (WOR verici, REG3 = `0x0B`), 9600 baud'da `wake` (veya `uyan`) gönder.

> Not: HAT'in Raspberry Pi başlığındaki RX/TX etiketleri **Pi'nin** bakış açısına göre yazılmış.

## Sonuçlar

### Akım (sadece MCU, JP6 üzerinden)

| Mod | Ölçülen |
|---|---|
| Stop 2, bare-metal | **1–2 µA** |
| FreeRTOS, tickless kapalı | ~1 mA |
| FreeRTOS + tickless + LPTIM | **20–30 µA** (~40 kat düşük) |
| Ana güç AÇIK (LED yük ~9 mA) | MCU hâlâ Stop 2'de uyuyor |

### Görev döngüsü
- Olaysız 120 s: **0.3 s uyanık → %0.27**
- ~25 ses olayıyla 1040 s: **%0.75**

### LoRa ile uyanma
- WOR periyodu 2000 ms → uyanma gecikmesi **1–2 s**

| Yapılandırma | Sonuç | UART hatası | Stop 2 uyanması / mesaj |
|---|---|---|---|
| LPUART1 LSE, 9600 baud | mesajların çoğu bozuk | 88 / 172 bayt | – |
| LSE, 4800 baud | 7–8 / 10 uyandırdı, mesaj ortasında harf kaybı | ~5 / 73 bayt | ~14 |
| LSE, 4800 baud + 200 ms alış penceresi | **10 komut eksiksiz alındı, hepsi ana gücü açtı** | **0 / 88 bayt** | **2** |

### Ses sınıflandırma (ana güç kararı)

| Veri seti | Sonuç |
|---|---|
| Eğitim verisi | eşikler 32 örnekle ayarlandı |
| **Test verisi (yeni örnekler)** | **13/15 = %87** doğru güç kararı (1 kaçırılan alkış, 1 yanlış alarm) |

Analiz süresi: ~1.43 M çevrim ≈ 4 MHz'te 358 ms (`-O0`).

Ham verilerle tüm ölçüm kayıtları: [docs/OLCUMLER.md](docs/OLCUMLER.md)

## Öğrenilenler

1. **Aynı RF çipi ≠ uyumlu telsizler.** Waveshare USB-TO-LoRa (DTU firmware'i) ile E22 tabanlı HAT, 8 farklı hava hızında hiç haberleşmedi.
2. **LPUART saat kaynağı önemli.** PCLK1 ile LPUART Stop 2'de durur; almak ve uyandırmak için **LSE**'den beslenmeli.
3. **LSE ile 9600 baud'da bit başına sadece ~3.4 saat darbesi** düşer, gürültü/çerçeve hataları sıklaşır. Düğüm HAT ↔ MCU UART hızını **4800 baud**'a (~6.8 darbe/bit) düşürmek durumu iyileştirdi; LoRa hava hızı değişmedi. Ayrıca gürültü bayrağı geldiğinde satır tamponu atılmamalı.
4. **Baytlar arasında Stop 2'ye girmek bayt kaybettirir.** MCU her bayttan sonra Stop 2'ye dönüyordu (mesaj başına ~14 uyanma) ve mesajın ortasından harf kaybediyordu. Test için Stop 2 kapatılınca hatalar tamamen kayboldu; sorunun saat olmadığı böylece kanıtlandı. Çözüm: LPUART uyanma geri çağrısıyla başlayan ve her baytla uzayan **200 ms alış penceresi**; bu sürede boşta Stop 2 yerine sadece WFI kullanılır.
5. **WFI öncesi SysTick yarışı** sebepsiz uyanışlara yol açıyordu. SysTick durdurulduktan sonra `PENDSTSET` kontrol edilerek çözüldü.
6. **CubeMX, ADC için geçersiz bir PLLSAI1 ayarı üretti** → `Error_Handler()`. ADC saati SYSCLK'a alındı.
7. **Aşırı uyum (overfitting):** sınıflandırıcı ayarlandığı veride 22/22, yeni veride 13/15 başarı gösterdi.

## Derleme

1. Projeyi **STM32CubeIDE**'de aç (File → Import → Existing Projects into Workspace).
2. `Debug` olarak derle ve NUCLEO-L476RG'ye yükle.
3. ST-LINK sanal COM portunu 115200 baud'da aç.

Önemli dosyalar:
- [`Core/Src/tickless_lptim.c`](Core/Src/tickless_lptim.c): LPTIM1 + Stop 2 ile tickless idle
- [`Core/Src/main.c`](Core/Src/main.c): olay işleme, ses sınıflandırma, LoRa, güç anahtarlama
- [`GhostSentinel.ioc`](GhostSentinel.ioc): CubeMX ayarlarının tamamı

## Yol haritası

- [ ] Uçuş kontrolcüsü için yüksek taraf anahtarlama (P-MOSFET + N-MOSFET sürücü + RC yumuşak başlatma)
- [ ] Tickless modundaki ek akımın kaynağını bulmak (bare-metal 1–2 µA'ya karşı 20–30 µA)
- [ ] LoRa modülü akımı ↔ WOR periyodu ↔ gecikme
- [ ] LoRa güvenliği: testte aynı kanaldaki yabancı bir paket alındı → ağ kimliği, E22 şifreleme anahtarı, doğrulamalı komutlar
- [ ] `-O2` ile analiz süresi, daha az Goertzel frekansı / FFT karşılaştırması
- [ ] MAVLink üzerinden PX4 uçuş kontrolcüsü entegrasyonu

## Lisans

MIT
