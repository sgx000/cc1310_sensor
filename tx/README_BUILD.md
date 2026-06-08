# CC1310 Gadget — instrukcja budowy

Niskoenergetyczny węzeł nadawczy dla LAUNCHXL-CC1310 Rev A.
NoRTOS + EasyLink + standby (Power_idleFunc).

## Co robi

- Po starcie: 3× mignięcie obu LED-ów + power-on TX
- Każda zmiana stanu BTN-1 (DIO13) lub BTN-2 (DIO14) wybudza chip
  ze standby, wysyła ramkę do gateway-a z powodem wybudzenia,
  mruga obiema diodami, wraca do standby
- Raz na 30 min bez aktywności: heartbeat (RTC wybudza)
- Pomiędzy zdarzeniami: deep standby

Adres węzła jest wyliczany z fabrycznego IEEE ID chipa (unikalny per
sztuka). Napięcie zasilania mierzone przez AON BatMon przy każdej ramce.

## Wymagania — patrz README gatewaya

To samo: SDK 4.20, GCC 12.3, poprawiony makefile.

W tym wariancie modyfikujemy też plik `easylink/EasyLink_nortos.c` —
dodajemy funkcję `EasyLink_close()` (oryginał TI tego nie udostępnia).
Pozwala to pełnie zamknąć RF między zdarzeniami żeby radio drivera
nie zostawiał aktywnych zegarów wewnętrznych.

Modyfikujemy też `CC1310_LAUNCHXL.c` — wyłączamy okresową kalibrację
RCOSC w `PowerCC26XX_config` (`calibrateRCOSC_LF/HF = false`), bo płytka
ma kryształ 32 kHz XOSC_LF i kalibracja jest zbędna a tylko budzi chip.

## Budowa

```bash
cd gcc
make COM_TI_SIMPLELINK_CC13X0_SDK_INSTALL_DIR=/scieżka/do/simplelink_cc13x0_sdk_4_20_02_07
```

Wynik: `rfEasyLinkTx.out`. Konwersja do hex jak w gatewayu.

## Format ramki (12 bajtów)

```
[0]      node addr  - 1 bajt, wyliczony z IEEE chip ID
[1]      reason     - 0=poweron, 1=btn1, 2=btn2, 3=heartbeat
[2]      btn1 state - 0=zwolniony, 1=wcisniety
[3]      btn2 state
[4..7]   chip ID    - 4 bajty z fabrycznego IEEE
[8][9]   frame seq  - hi/lo
[10][11] vbat_mv    - napiecie zasilania w miliwoltach
```

Adres docelowy ramki w polu `dstAddr` to `0xAA` — gateway ma włączony
filtr adresów na `0xAA` (`EASYLINK_ADDR_FILTER_TABLE = {0xAA}`).

## Pobór prądu

Średnio ~29 µA z aktualną konfiguracją. Krótkie szpilki ~10 mA podczas
TX i okresowe drobne ~5-15 mA z aktywności wewnętrznej chipa
(prawdopodobnie AON event fabric). Deep standby baseline udokumentowany
na 300-700 nA w teście minimalnym.

CR2032 (220 mAh) przy 29 µA → ~10 miesięcy autonomii.
AA litowa (3000 mAh) → ~12 lat.

## Pliki

- `rfEasyLinkTx_nortos.c` — główna logika gadgetu (mój kod)
- `easylink/EasyLink_nortos.c` — **MODYFIKOWANY**: dodana funkcja
  `EasyLink_close()` (w stocku TI nie ma)
- `CC1310_LAUNCHXL.c` — **MODYFIKOWANY**: kalibracja RCOSC wyłączona
- `main_nortos.c` — entry point NoRTOS (stock TI)
- `Board.h`, `CC1310_LAUNCHXL.h`, `CC1310_LAUNCHXL_fxns.c` — stock TI
- `ccfg.c` — stock TI
- `easylink/EasyLink.h`, `easylink_config.c/.h` — stock TI
- `smartrf_settings/` — stock TI (PHY 50 kbps GFSK)
- `gcc/makefile` — z poprawioną ścieżką bibliotek
- `gcc/CC1310_LAUNCHXL_NoRTOS.lds` — linker script (stock TI)
