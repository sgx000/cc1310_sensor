# CC1310 Gateway — instrukcja budowy

Gateway dla LAUNCHXL-CC1310 Rev A. NoRTOS + EasyLink.
Odbiera ramki radiowe na 868 MHz i wysyła je jako JSON przez UART
(XDS110 backchannel, 115200 8N1).

## Wymagania

1. **SimpleLink CC13x0 SDK 4.20.02.07** zainstalowany na dysku.
   Powinien zawierać katalog `kernel/nortos`, `source/ti/drivers`,
   `source/ti/devices/cc13x0/driverlib/bin/gcc/driverlib.lib` itd.
2. **arm-gnu-toolchain 12.3** (lub kompatybilny GCC ARM bare-metal).
   Linux x86_64 albo Windows — w obu da się zbudować, ścieżki dostosować
   w `imports.mak` SDK.

## Krytyczna poprawka makefile

Domyślny makefile TI ma błędną ścieżkę bibliotek dla GCC 12+:
zamiast `arm-none-eabi/lib/thumb/v7-m` powinno być
`arm-none-eabi/lib/thumb/v7-m/nofp`. Bez tego linker wciąga
ARM-owe wersje funkcji bibliotecznych (memcpy, snprintf...), których
Cortex-M3 nie umie wykonać i firmware się zawiesza.

W archiwum `gcc/makefile` ma już poprawioną ścieżkę.

## Budowa

```bash
cd gcc
make COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR=/scieżka/do/simplelink_cc13x0_sdk_4_20_02_07
```

Wynik: `rfEasyLinkRx.out` (ELF). Konwersja do .hex:
```bash
arm-none-eabi-objcopy -O ihex rfEasyLinkRx.out gateway.hex
```

UWAGA — `objcopy -O ihex` użyje formatu Intel HEX z type-02 (Extended
Segment Address) records dla adresów > 64 KB. UniFlash dla CC1310
PRAWIDŁOWO interpretuje TYLKO type-04 (Extended Linear Address).
CCFG na 0x1FFA8 nie zostanie poprawnie zapisany.

Workaround: użyj `arm-none-eabi-objcopy -O binary` + pad-to + skrypt
konwersji do Intel HEX z type-04 (patrz `tools/bin2hex.py` jeśli
dołączony, albo po prostu wgraj plik `.out` (ELF) przez CCS — CCS
natywnie obsługuje ELF i poprawnie adresuje sekcje.

## Wgranie

UniFlash:
- Device: CC1310F128
- File: `gateway.hex` (po konwersji do Intel HEX z type-04) albo `rfEasyLinkRx.out`
- "Erase entire flash" zaznaczone
- Load Image
- Power-cycle USB

CCS: Run -> Load Program -> wskazać `rfEasyLinkRx.out`.

## Działanie

Po starcie (sygnalizacja wyłącznie przez UART/OLED — **brak diod LED**):
1. `{"type":"boot","info":"CC1310 gateway started","fw":"1.0.0","sw":"1.0.0"}`
2. `{"type":"info","oled":"ssd1306 ready"}` (lub `warn` gdy panel nieobecny)
3. `{"type":"ready","freq_hz":868000000,"phy":"custom_50kbps_gfsk"}`
4. Czeka na pakiety radiowe lub heartbeat co 60 s (`HEARTBEAT_TIMEOUT_MS`).

Format wyjścia dla ramki gadgetu (`len=14`):
```json
{"type":"node","rxseq":23,"rssi":-22,"node":"9D","reason":"btn1",
 "btn1":1,"btn2":0,"btn3":0,"chipId":"12345678","fseq":15,
 "vbat_mv":3300,"temp":23}
```

## Przycisk

Jeden przycisk: **DIO14** (`Board_PIN_BTN2`). Drugi przycisk i diody LED
zostały usunięte (cały status jest na OLED).

- Wciśnięcie **włącza OLED**: najpierw ekran startowy z wersją (`CC1310 Gateway`,
  `FW`, `SW`, liczba nodów), potem urządzenia z tabeli **po kolei** (jedno na
  ekran, ~10 s każde): node, online/offline + RSSI, bateria %/mV, stan switchy.
- Urządzenie **offline** pokazywane jest w **negatywie** (inwersja całego panelu,
  komenda `0xA7`) dla lepszej widoczności; online — normalnie (`0xA6`).
- Kolejne **wciśnięcie = skip** do następnego ekranu (pomija 10 s oczekiwania);
  na ostatnim ekranie skip kończy cykl i **gasi panel**.
- Parametry: czas ekranu `OLED_PAGE_MS`, wersje `FW_VERSION` / `SW_VERSION`.

## Tabela nadajników

- Do **30** nadajników, kluczowane po `node addr` (`payload[0]`).
- Dla każdego: ostatni stan switchy, `vbat_mv`, `fseq`, RSSI, czas ostatniej ramki.
- **Bateria**: `>= 3.0 V` → 100%, `<= 2.2 V` → 0%, pomiędzy liniowo
  (`BATT_FULL_MV` / `BATT_EMPTY_MV`).
- **Online**: urządzenie jest online, jeśli ostatnia ramka przyszła w ciągu
  `ONLINE_TIMEOUT_MS` (30 min — tyle co cykl heartbeat nadajnika). Czas jest
  pochodną okien RX.

## OLED SSD1306 (128x32, I2C)

Podłączenie do LAUNCHXL (Board_I2C0):

| OLED | CC1310 LaunchXL |
|------|-----------------|
| SCL  | DIO4            |
| SDA  | DIO5            |
| VCC  | 3V3             |
| GND  | GND             |

Adres I2C: `0x3C` (gdyby panel miał `0x3D`, zmień `SSD1306_I2C_ADDR` w `ssd1306.h`).
Sterownik (`ssd1306.c/.h`) ma wbudowany font 5x7 i framebuffer w RAM (512 B).
Brak panelu nie jest błędem krytycznym — gateway działa dalej, a BTN2 zgłasza
`{"type":"warn","info":"OLED not present"}`.

## Pliki

- `rfEasyLinkRx_nortos.c` — główna logika gatewaya (mój kod)
- `ssd1306.c` / `ssd1306.h` — sterownik OLED SSD1306 128x32 po I2C (mój kod)
- `main_nortos.c` — entry point NoRTOS (stock TI)
- `Board.h`, `CC1310_LAUNCHXL.c/.h`, `CC1310_LAUNCHXL_fxns.c` — board files (stock TI)
- `ccfg.c` — CCFG config (stock TI)
- `easylink/` — EasyLink wrapper (stock TI)
- `smartrf_settings/` — PHY config (stock TI, SmartRF Studio output)
- `gcc/makefile` — z poprawioną ścieżką bibliotek (+ reguła `ssd1306.obj`)
- `gcc/CC1310_LAUNCHXL_NoRTOS.lds` — linker script (stock TI)
