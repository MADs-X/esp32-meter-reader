# ESP32-S3 CC1312 Meter Gateway

ESP32-S3 tabanlı, CC1312 Sub-GHz radyo modülü ile haberleşen ve akıllı sayaçlardan (Meter-Read protokolü) veri toplayıp SQLite ile kaydeden bir IoT Gateway.

## Özellikler
- CC1312 ile UART üzerinden SLIP/6LoWPAN iletişimi
- UDP sunucuları (port 8004, 8006, 8007, 8008)
- Meter-Read protokolü (MR_CONFIGURE, MR_DATA, MR_MISS, MR_READ_ERROR)
- LittleFS + SQLite ile veri kalıcılığı
- FreeRTOS görev tabanlı asenkron veritabanı yazma

## Donanım Bağlantıları
| ESP32-S3 Pin | CC1312 Pin | Açıklama |
|--------------|------------|----------|
| GPIO17       | TX         | UART Veri Gönderme |
| GPIO18       | RX         | UART Veri Alma |
| GPIO5        | RESET      | CC1312 Reset Kontrolü |

