# Ghost Sentinel Lite — Ölçüm ve Test Kayıtları

*English: [MEASUREMENTS.md](MEASUREMENTS.md)*

Donanım: NUCLEO-L476RG (STM32L476RG, MSI 4 MHz, Range 2) · Ses modülü (elektret + LM393, AO/DO) ·
Waveshare SX1262 868M LoRa HAT ×2 (Ebyte E22-900T22S) · FreeRTOS (CMSIS-RTOS2) · Ölçüm: multimetre, JP6 (IDD)

---

## 1. Akım ölçümleri (yalnızca MCU, JP6 üzerinden)

| Mod | Ölçülen | Not |
|---|---|---|
| Sleep modu (CPU durur, saatler açık) | ~800 µA | |
| **Stop 2, bare-metal** (EXTI ile uyanma) | **1–2 µA** | Rapordaki teorik değerle uyumlu |
| FreeRTOS, tickless **kapalı** | ~1 mA | Her 1 ms SysTick ile uyanıyor |
| FreeRTOS, debugger bağlıyken | ~70 µA | Debug birimi Stop'ta açık kalıyor |
| **FreeRTOS + tickless idle (LPTIM1)** | **20–30 µA** | ~40× iyileşme. Bare-metal'e göre fark açık konu (bkz. §6) |
| MCU uyanık (4 MHz) | 1.2 mA |
| Ses modülü (LED'li) | 3 mA | 
| LoRa HAT (WOR / Mod 0 / uyku) | 0.5 mA / 16mA / 2-3 uA | 

### 1.1 Beklenen değerler (datasheet + tahmin, ölçülmedi)

Bu tablo ölçüm değil; ölçümlerin hangi aralıkta çıkması gerektiğini gösteren bir referans. Değerler 3.3 V (ses modülü) ve 5 V (HAT) beslemeye göre verildi.

| Bileşen / mod | Beklenen | Dayanak |
|---|---|---|
| MCU çekirdeği, Run, 4 MHz, Range 2 | ~0.4–0.5 mA | STM32L476 datasheet (~100 µA/MHz) |
| MCU, bu karttaki gerçek uyanık hali | ~1–1.5 mA | Sleep'te zaten ~0.8 mA ölçüldü (açık çevre birimi saatleri); Run bunun üstünde olmalı |
| MCU, ses ölçümü sırasında (ADC + DMA + TIM6) | ~1.5–2 mA, 128 ms boyunca | Run + ADC akımı |
| Ses modülü: LM393 | ~0.4 mA (en çok 1 mA) | LM393 datasheet |
| Ses modülü: elektret mikrofon bias | ~0.2–0.5 mA | tipik ~10 kΩ bias direnci |
| Ses modülü: güç LED'i (kırmızı) | ~1–2 mA | 3.3 V, ~1 kΩ seri direnç varsayımı |
| Ses modülü: pot (VR1) bölücüsü | ~0.1–0.3 mA | 10 kΩ civarı pot varsayımı |
| **Ses modülü toplam (boşta)** | **~2–4 mA** | yukarıdakilerin toplamı; DO LED'i yanınca +1–2 mA |
| E22-900T22S, Mod 0 sürekli alım | ~15–17 mA | Ebyte kullanım kılavuzu |
| E22-900T22S, gönderim (22 dBm) | ~110–150 mA tepe | Ebyte kullanım kılavuzu (düğüm göndermiyor) |
| E22-900T22S, derin uyku (Mod 3) | ~2–3 µA | Ebyte kullanım kılavuzu |
| E22-900T22S, WOR alıcı, 2000 ms periyot | ~0.1–0.5 mA ortalama | tahmin: her periyotta kısa bir dinleme penceresi; datasheet'te verilmiyor |
| Waveshare HAT kartının kendi yükü (LED, regülatör) | +1–5 mA | tahmin; HAT'ten ölçülen değer modülden yüksek çıkabilir |

**Beklenen sistem bütçesi (boşta, nöbet modunda):**

| Kalem | Ortalama akım |
|---|---|
| MCU (tickless, ölçüldü) | 0.02–0.03 mA |
| Ses modülü (LED'li, tahmin) | 2–4 mA |
| LoRa HAT, WOR (tahmin) | 1–5 mA |
| **Toplam** | **~3–9 mA** |

**Çıkarım:** MCU tarafı µA seviyesine indi, ama hazır modüller sistemi mA seviyesinde tutuyor. Ses modülü ve HAT, MCU'dan ~100–300 kat fazla çekiyor. Örneğin 2000 mAh'lik bir pil yalnızca MCU ile yıllarca, modüllerle birlikte ise yaklaşık 10–30 gün dayanır. Sonraki iyileştirmeler:
- Ses modülündeki güç LED'ini sökmek (~1–2 mA kazanç)
- LM393 yerine nanoamper seviyesinde bir karşılaştırıcı ya da "wake-on-sound" mikrofon kullanmak
- Waveshare HAT yerine çıplak E22 modülünü doğrudan beslemek ve WOR periyodunu uzatmak (gecikme ↔ akım dengesi)
- Modüllerin beslemesini MCU'nun kontrol ettiği bir anahtarla kesmek

## 2. Görev döngüsü (duty cycle) — log'lardan

| Durum | Gerçek süre | Uyanık süre | Görev döngüsü |
|---|---|---|---|
| Bare-metal + LPTIM nabız (Adım 4) | 110 s | 3.7 s | %3.4 (3.5 s'si ana güç penceresi) |
| Tickless, olay yok | 120 s | 0.325 s | **%0.27** |
| Tickless + 25 ses olayı (ses analizi açık) | 1040 s | 7.8 s | %0.75 |

## 3. Tickless idle doğrulama

Örnek log (Adım 5b): `[120007 ms] NABIZ ... uyanik=325 ms | stop2=19 (zaman=17 olay=2) uyku=119725 ms`
→ 120 s'nin 119.7 s'si Stop 2'de; FreeRTOS zamanı LPTIM ile kayıpsız tutuluyor (zaman damgaları 10 s adımlı düzenli).

## 4. LoRa testleri

| Test | Sonuç |
|---|---|
| USB-TO-LoRa-HF (DTU) ↔ SX1262 HAT (E22), HAT hava hızı 0.3k/1.2k/2.4k/4.8k/9.6k/19.2k/38.4k/62.5k | **Hiçbirinde iletişim yok** (iki yönde) — aynı çip, farklı üretici firmware'i |
| HAT ↔ HAT, Mod 0 | ✅ İletişim var |
| HAT (WOR verici, REG3=0x0B) → HAT (WOR alıcı, REG3=0x03), WOR periyodu 2000 ms | ✅ Mesaj **1–2 s** gecikmeyle ulaşıyor. REG3 bit3: 1 = verici, 0 = alıcı (Ebyte dokümanı doğru) |
| HAT → Nucleo LPUART1, Stop 2'den uyanma | ✅ (aşağıdaki düzeltmelerden sonra) |

### Bulunan ve çözülen sorunlar
1. HAT'in Pi başlığındaki RX/TX etiketleri **Raspberry Pi'ye göre** yazılmış (HAT "RX" = modülün çıkışı).
2. LPUART1 saat kaynağı **PCLK1** iken Stop 2'de saat durur, bayt alınamaz → **LSE** seçildi.
3. RXNE yerine **start bit** ile uyanma + **WUF kesmesi** açıldı (ST'nin önerdiği yöntem).
4. Bayt alınırken Stop 2'ye girilmemesi için `USART_ISR_BUSY` kontrolü eklendi.
5. LSE (32.768 kHz) ile 9600 baud'da bit başına ~3.4 saat darbesi → gürültü/çerçeve hataları; hata geri çağrısında tampon sıfırlanmamalı. Düğüm HAT ↔ MCU UART hızı **4800 baud**'a düşürüldü (REG0 = `0x42`, ~6.8 darbe/bit).
6. CubeMX, ADC saati için geçersiz PLLSAI1 ayarı üretti → `Error_Handler`; ADC saati SYSCLK yapıldı.
7. **Mesaj ortasında bayt kaybı** (`uya`, `uan`, `ua`): MCU her bayttan sonra yeniden Stop 2'ye giriyordu (mesaj başına ~14 uyanma). Teşhis testi: Stop 2 geçici olarak kapatıldığında (`tl_stop_lock = 1`) hiç hata olmadı → sorun UART saati değil, Stop 2 geçişleri.
8. Çözüm: **alış penceresi**. `HAL_UARTEx_WakeupCallback` (start bitiyle uyanma) ve her alınan bayt `tl_hold_until = şimdi + 200 ms` yapar; bu süre boyunca tickless idle Stop 2 yerine sadece WFI (Sleep) kullanır. Komut eşleştirme `strstr` ile yapılır.
9. Teşhis sayaçları (`dbg busy / hold / lock`): Stop 2'nin neden atlandığını gösterir. Unutulan bir test satırını (`lock` her ms artıyordu) bu sayaç yakaladı.

### 4.1 Güvenilirlik (HAT → Nucleo, WOR)

| Yapılandırma | Sonuç | `err` | Stop 2 uyanması / mesaj |
|---|---|---|---|
| LPUART1 LSE, 9600 baud | Mesajların çoğu bozuk | 88 / 172 bayt | – |
| LSE, 4800 baud | 7–8 / 10 uyandırdı, ortadan harf kaybı | ~5 / 73 bayt | ~14 |
| LSE, 4800 baud + 200 ms alış penceresi | **10 komut (3× `uyan`, 7× `wake`) eksiksiz alındı, hepsi ana gücü açtı** | **0 / 88 bayt** | **2** |

- Bayt sağlaması: 10 × 6 bayt (`komut\r\n`) + 28 bayt yabancı paket = 88 = `lora_bytes` → gelen her mesaj tam.
- Boştayken uyanık oranı ≈ %0.4 (iki nabız arasında ~35 ms uyanık); pencere mesaj başına ~0.6 s hafif uyku (Sleep) maliyeti getiriyor.
- Log (150 s): `lora=11 | awake=5245 ms | stop2=47 (timer=25 event=22) sleep=144857 ms | lora_bytes=88 err=0 | dbg busy=315 hold=7093 lock=0`
- Not: Bu turda gönderilen toplam komut sayısı ayrıca kaydedilmedi; ölçülen, **alınan mesajların UART seviyesinde kayıpsız** olduğudur. Havada (WOR) kayıp oranı ayrı bir testle ölçülmeli.

### 4.2 Güvenlik gözlemi
Aynı kanalda başka bir cihazın paketi yakalandı: `{"target":"grp_1","poll":1}` (pencereden önce `{"targe":"gp_1","poll":}` olarak bozuk geliyordu). Kod tanımadığı komutu reddetti, ama aynı kanalda `uyan` gönderen herkes nöbetçiyi uyandırabilir → yol haritası: NETID/adres, E22 şifreleme anahtarı (REG 07–08), doğrulamalı komut (sıra no + sağlama).

## 5. Ses sınıflandırma

Örnekleme: TIM6 → ADC1 (PA0) → DMA, 8 kHz, 1024 örnek (128 ms). Özellikler:
- **tepe/ort**: 16 × 8 ms blok enerjisinde maks/ortalama (zamandaki şekil)
- **tiz %**: Goertzel ile 100–3000 Hz (100 Hz adım) taramasında ≥1 kHz enerji oranı
- Analiz süresi: **~1.43 M çevrim = 358 ms @ 4 MHz, `-O0`** (`-O2` ölçülecek)

### Karar ağacı (son hali)
```
tepe/ort ≥ 2.7 ?  → EVET: tiz ≥ %50 ? → ALKIŞ (güç AÇ) / değilse VURMA (yanlış alarm)
                  → HAYIR: tepe/ort ≤ 2.5 VE tiz ≥ %50 ? → MOTOR (güç AÇ) / değilse GENEL (yanlış alarm)
```
(Test seti 3.5 / 2.0-tiz'siz eşiklerle alındı; 2.7 düzeltmesi sonradan önerildi.)

### 5.1 Ham veriler — Eğitim seti

| # | Ses | rms (mV) | tepe-tepe (mV) | tepe/ort | f_dom (Hz) | tiz % |
|---|---|---|---|---|---|---|
| a1 | Alkış | 2 | 61 | 5.4 | – | – |
| a2 | Alkış | 7 | 77 | 6.9 | – | – |
| a3 | Alkış | 3 | 37 | 2.9 | – | – |
| a4 | Alkış | 15 | 227 | 6.8 | – | – |
| a5 | Alkış | 7 | 83 | 4.2 | – | – |
| a6 | Alkış | 15 | 137 | 3.6 | – | – |
| a7 | Alkış | 5 | 60 | 4.0 | 1700 | 91 |
| a8 | Alkış | 25 | 255 | 4.9 | 1900 | 91 |
| a9 | Alkış | 6 | 68 | 4.1 | 1900 | 83 |
| a10 | Alkış | 6 | 58 | 3.6 | 500 | 64 |
| a11 | Alkış | 29 | 289 | 5.5 | 1400 | 80 |
| a12 | Alkış | 5 | 48 | 3.6 | 2000 | 78 |
| a13 | Alkış | 24 | 220 | 4.2 | 1800 | 87 |
| v1 | Vurma | 4 | 53 | 7.3 | – | – |
| v2 | Vurma | 6 | 98 | 8.3 | – | – |
| v3 | Vurma | 3 | 29 | 4.7 | – | – |
| v4 | Vurma | 9 | 132 | 9.5 | 100 | 10 |
| v5 | Vurma | 10 | 125 | 6.5 | 400 | 22 |
| v6 | Vurma | 6 | 76 | 7.6 | 100 | 6 |
| v7 | Vurma | 10 | 96 | 6.6 | 100 | 16 |
| v8 | Vurma | 13 | 138 | 6.1 | 100 | 9 |
| k1 | Konuşma | 2 | 21 | 2.3 | – | – |
| k2 | Konuşma | 68 | 404 | 1.7 | 600 | 6 |
| k3 | Konuşma | 17 | 130 | 2.3 | 300 | 2 |
| k4 | Konuşma | 36 | 265 | 2.4 | 1000 | 46 |
| k5 | Konuşma | 34 | 222 | 2.8 | 600 | 1 |
| m1 | Motor | 3 | 46 | 2.3 | 600 | 58 |
| m2 | Motor | 17 | 111 | 1.4 | 1500 | 94 |
| m3 | Motor | 20 | 137 | 1.5 | 1500 | 96 |
| m4 | Motor | 18 | 103 | 1.3 | 2000 | 98 |
| m5 | Motor | 16 | 99 | 1.4 | 1900 | 90 |
| m6 | Motor | 17 | 116 | 1.5 | 1200 | 88 |

Not: Uzaktan normal konuşma DO eşiğini aşmadı → kart hiç uyanmadı (donanım ön filtresi).

### 5.2 Ham veriler — Test seti (kural güncellendikten sonra)
Etiketler sıraya göre çıkarıldı (alkış → vurma → konuşma → motor); doğrulanmalı.

| # | Ses | rms | tepe-tepe | tepe/ort | f_dom | tiz % | Karar | Güç kararı |
|---|---|---|---|---|---|---|---|---|
| 2 | Alkış | 7 | 84 | 5.0 | 1200 | 81 | ALKIŞ | ✅ |
| 3 | Alkış | 9 | 117 | 4.4 | 1300 | 74 | ALKIŞ | ✅ |
| 4 | Alkış | 5 | 53 | 3.0 | 1200 | 77 | GENEL | ❌ kaçırma |
| 5 | Alkış | 20 | 224 | 4.9 | 1500 | 78 | ALKIŞ | ✅ |
| 6 | Vurma | 15 | 262 | 8.1 | 100 | 12 | VURMA | ✅ |
| 7 | Vurma | 4 | 36 | 4.9 | 100 | 26 | VURMA | ✅ |
| 8 | Vurma | 6 | 54 | 2.4 | 100 | 37 | GENEL | ✅ (güç açılmadı) |
| 9 | Konuşma | 25 | 194 | 2.1 | 300 | 28 | GENEL | ✅ |
| 10 | Konuşma | 3 | 45 | 1.9 | 2400 | 66 | MOTOR | ❌ yanlış alarm |
| 11 | Konuşma | 21 | 167 | 1.8 | 600 | 5 | GENEL | ✅ |
| 12 | Konuşma | 19 | 152 | 2.0 | 600 | 32 | GENEL | ✅ |
| 13 | Motor | 8 | 53 | 1.5 | 1500 | 94 | MOTOR | ✅ |
| 14 | Motor | 27 | 198 | 1.2 | 2300 | 98 | MOTOR | ✅ |
| 15 | Motor | 13 | 87 | 1.4 | 1600 | 96 | MOTOR | ✅ |
| 16 | Motor | 15 | 98 | 1.7 | 1900 | 97 | MOTOR | ✅ |

### 5.3 Karışıklık matrisleri

**Eğitim (ilk kural, n=15, son tam oturum):** 14/15 = %93 (tek hata: konuşma → MOTOR)
**Eğitim (güncellenmiş kural, tüm tiz'li veriler):** 22/22 — aynı veriyle ayarlandığı için iyimser (aşırı uyum)

**Test (güncellenmiş kural, yeni veri, n=15):**

| Gerçek ↓ / Karar → | ALKIŞ | VURMA | MOTOR | GENEL |
|---|---|---|---|---|
| Alkış (4) | 3 | 0 | 0 | 1 |
| Vurma (3) | 0 | 2 | 0 | 1 |
| Konuşma (4) | 0 | 0 | 1 | 3 |
| Motor (4) | 0 | 0 | 4 | 0 |

- 4 sınıf doğruluğu: **12/15 = %80**
- Ana güç kararı doğruluğu: **13/15 = %87** (1 kaçırma, 1 yanlış alarm)
- Hata analizi: (a) alkış #4 — geç uyanma, tepe kaçtı (tepe/ort 3.0); (b) konuşma #10 — çok zayıf sinyal (rms 3 mV), spektrum gürültüye kaydı.
- Önerilen düzeltme: darbe eşiği 3.5 → 2.7 (yeni test seti ile doğrulanmalı).

## 6. Açık konular
- Tickless akımı 20–30 µA, bare-metal Stop 2 ise 1–2 µA: Test A (Adım 4 bare-metal+LPTIM ölçümü) ve Test C (nabız kapalı) yapılacak.
- `-O0` → `-O2` analiz süresi karşılaştırması.
- MOSFET/röle ile gerçek yük; yük kolu akımı.
- LoRa HAT akımı (Mod 0 / WOR periyotları / uyku) ve WOR periyodu ↔ gecikme ↔ akım tablosu.
- LoRa havada kayıp oranı (gönderilen / alınan, sayılı test) ve LoRa güvenliği (şifreleme anahtarı).
