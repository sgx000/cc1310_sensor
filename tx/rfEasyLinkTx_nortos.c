/*
 * Gadget CC1310 LAUNCHXL Rev A - NoRTOS, ULTRA low-power.
 *
 * Wzorzec: NIE init RF na starcie. Per event (button/heartbeat):
 *   - EasyLink_init + setFrequency
 *   - BATMON enable, pomiar, disable
 *   - EasyLink_transmit
 *   - EasyLink_close (RF_close + reset state)
 *   - LED blink
 *   - powrot do standby
 * Miedzy zdarzeniami RF jest CALKOWICIE zamkniete - brak periodicznych
 * zegarow RF drivera (RAT sync, inactivity).
 */
#include "Board.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ti/drivers/pin/PINCC26XX.h>
#include <ti/drivers/PIN.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26XX.h>
#include <ti/drivers/dpl/ClockP.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/aon_batmon.h)

#include "easylink/EasyLink.h"

/* Dodana funkcja w EasyLink_nortos.c */
extern EasyLink_Status EasyLink_close(void);

#define GATEWAY_ADDR        0xAA
#define RF_FREQUENCY_HZ     868000000U
#define HEARTBEAT_MINUTES   30U
#define WAKE_POWERON   0
#define WAKE_BTN1      1
#define WAKE_BTN2      2
#define WAKE_HEARTBEAT 3
#define WAKE_BTN3      4

#define LED_BLINK_LOOPS  1600000U
#define DEBOUNCE_LOOPS    500000U
#define JITTER_MAX_MS        100U   /* anty-kolizja: losowy odstep 0..100 ms przed TX */

/* --- Mapowanie pinow na wlasnej plytce z modulem RF-CC1310 ---
 * 1 dioda na DIO6 (active-high: DIO6 -> R -> LED -> GND)
 * 3 kontaktrony: DIO7, DIO8, DIO9 (zwarcie do GND = aktywny). */
#define APP_PIN_LED   IOID_6
#define APP_PIN_BTN1  IOID_7
#define APP_PIN_BTN2  IOID_8
#define APP_PIN_BTN3  IOID_9

/* Pull na wejsciach kontaktronow:
 *  0 = wewnetrzny pull-up (~40k) - dziala bez elementow zewnetrznych, ALE przy
 *      zwartym kontaktronie plynie ~75 uA na linie (zabija budzet baterii!).
 *  1 = bez pulla wewnetrznego (PIN_NOPULL) - WYMAGA zewnetrznych pull-upow
 *      1-10 MOhm na kazdej linii; wtedy zwarty kontaktron to ~0.3-3 uA.
 * Na bring-up zostaw 0; do produkcji daj zewn. pull-upy i ustaw 1. */
#define REED_EXTERNAL_PULL  0
#if REED_EXTERNAL_PULL
  #define REED_PULL  PIN_NOPULL
#else
  #define REED_PULL  PIN_PULLUP
#endif

static PIN_Handle pinHandle;
static PIN_State  pinState;

PIN_Config pinTable[] = {
    APP_PIN_LED  | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,
    APP_PIN_BTN1 | PIN_INPUT_EN | REED_PULL | PIN_HYSTERESIS,
    APP_PIN_BTN2 | PIN_INPUT_EN | REED_PULL | PIN_HYSTERESIS,
    APP_PIN_BTN3 | PIN_INPUT_EN | REED_PULL | PIN_HYSTERESIS,
    PIN_TERMINATE
};

static volatile bool wakeFlag      = true;
static volatile bool heartbeatDue  = false;
static uint16_t      seqNumber     = 0;
static uint8_t       lastBtn1      = 0;
static uint8_t       lastBtn2      = 0;
static uint8_t       lastBtn3      = 0;
static uint8_t       nodeAddr      = 0x01;
static uint8_t       chipId[4]     = {0,0,0,0};
static bool          identityOk    = false;

static ClockP_Struct hbClockStruct;
static ClockP_Handle hbClock;
static uint32_t      hbTicks;

static ClockP_Struct blinkClkStruct;
static ClockP_Handle blinkClk;
static uint32_t      blinkTicks;
static uint32_t      ticksPerMs   = 100;     /* przeliczane w mainThread */
static volatile bool blinkDone    = false;
static volatile bool txDone       = false;

/* PRNG xorshift32 - wspolny dla jittera i backoffu CCA (rejestrowany jako pGrnFxn).
 * Seed z chipID -> kazdy wezel ma inny ciag, wiec dwa wezly rozjada sie na antenie. */
static uint32_t prngState = 0xA5A5A5A5u;
static uint32_t appRng(void){
    uint32_t x = prngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    prngState = x ? x : 0xA5A5A5A5u;
    return prngState;
}

static void busy(uint32_t n){ volatile uint32_t k=n; while(k--) __asm__ volatile("nop"); }

static void blinkClkCb(uintptr_t a){ (void)a; blinkDone = true; }
static void txDoneCb(EasyLink_Status s){ (void)s; txDone = true; }

/* Niskoenergetyczne czekanie: MCU spi w standby przez 'ticks' (budzi RTC).
 * Uzywane do jittera anty-kolizyjnego ORAZ do swiecenia diody. */
static void lpWaitTicks(uint32_t ticks){
    if(ticks==0) return;
    blinkDone = false;
    ClockP_stop(blinkClk);
    ClockP_setTimeout(blinkClk, ticks);
    ClockP_start(blinkClk);
    while(!blinkDone){ Power_idleFunc(); }
    ClockP_stop(blinkClk);
}

/* Blink: dioda swieci, a MCU spi w standby (stan GPIO podtrzymany) -> faza
 * aktywna po TX ~0, zostaje tylko prad diody. Czas: blinkTicks. */
static void blinkBothLeds(void){
    PIN_setOutputValue(pinHandle,APP_PIN_LED,1);
    lpWaitTicks(blinkTicks);
    PIN_setOutputValue(pinHandle,APP_PIN_LED,0);
}
static uint8_t readBtn(PIN_Id pin){ return PIN_getInputValue(pin)?0u:1u; }

/* Jeden seans BATMON: Vbat (mV) + temperatura ukladu (degC ze znakiem).
 * Czujnik temp jest w AON BATMON; mierzy temp KRZEMU (po standby ~= otoczenie),
 * dokladnosc rzedu kilku degC. Wynik w globalnych g_vbatMv / g_tempC. */
static uint16_t g_vbatMv = 0;
static int8_t   g_tempC  = 0;
static void readVbatAndTemp(void){
    AONBatMonEnable();
    uint32_t guard = 200000;
    while(!AONBatMonNewBatteryMeasureReady() && guard){ guard--; }
    uint32_t raw = AONBatMonBatteryVoltageGet();
    int32_t  t   = AONBatMonTemperatureGetDegC();
    AONBatMonDisable();
    uint32_t mv = (raw * 1000u) >> 8;
    if(mv > 65535u) mv = 65535u;
    g_vbatMv = (uint16_t)mv;
    if(t >  127) t =  127;
    if(t < -128) t = -128;
    g_tempC = (int8_t)t;
}

static void armButtonIrq(PIN_Id pin, uint8_t curState){
    PIN_setInterrupt(pinHandle, pin | (curState ? PIN_IRQ_POSEDGE : PIN_IRQ_NEGEDGE));
}

static void buttonCb(PIN_Handle h, PIN_Id pinId){
    (void)h; (void)pinId;
    wakeFlag = true;
}
static void hbClockCb(uintptr_t a){
    (void)a;
    heartbeatDue = true;
    wakeFlag     = true;
}

static void restartHeartbeatClock(void){
    ClockP_stop(hbClock);
    ClockP_setTimeout(hbClock, hbTicks);
    ClockP_start(hbClock);
}

static void deriveNodeIdentityFromIeee(void){
    uint8_t ieee[8]; memset(ieee,0,8);
    EasyLink_getIeeeAddr(ieee);
    /* ieee[0..2] = OUI Texas Instruments (stale dla kazdego chipa); unikalne
     * sa dolne bajty 64-bit adresu IEEE -> bierzemy ieee[4..7] (32 bity serii). */
    chipId[0]=ieee[4]; chipId[1]=ieee[5]; chipId[2]=ieee[6]; chipId[3]=ieee[7];
    /* Seed PRNG z unikalnego chipID -> jitter i backoff CCA inne na kazdym wezle. */
    prngState = ((uint32_t)chipId[0]<<24) ^ ((uint32_t)chipId[1]<<16)
              ^ ((uint32_t)chipId[2]<<8)  ^  (uint32_t)chipId[3] ^ 0xA5A5A5A5u;
    if(prngState==0) prngState = 0xA5A5A5A5u;
    uint8_t a=0; for(int i=0;i<8;i++) a^=ieee[i];
    if(a==0x00) a=0x01;
    if(a==GATEWAY_ADDR) a=(uint8_t)(a^0x37);
    if(a==0x00) a=0x42;
    nodeAddr=a;
}

/* Pelny cykl: init RF -> pobierz tozsamosc gdy pierwszy raz -> wyslij -> zamknij RF */
static void txWithFullRfCycle(uint8_t reason)
{
    /* Anty-kolizja #1: losowy odstep 0..JITTER_MAX_MS w standby, RF jeszcze
     * wylaczone (tanio). Dwa wezly wybudzone razem rozjada sie w czasie. */
    lpWaitTicks( (appRng() % (JITTER_MAX_MS + 1u)) * ticksPerMs );

    /* Pomiar Vbat+temp PRZED wlaczeniem RF: uklad najzimniejszy (prosto ze
     * standby), wiec temp krzemu najblizej otoczenia. BATMON nie wymaga RF. */
    readVbatAndTemp();

    EasyLink_Params elp;
    EasyLink_Params_init(&elp);
    elp.ui32ModType = EasyLink_Phy_Custom;
    elp.pGrnFxn     = appRng;             /* RNG wymagany przez CCA (backoff) */
    if(EasyLink_init(&elp) != EasyLink_Status_Success){
        /* awaria - migaj LED2 */
        PIN_setOutputValue(pinHandle,APP_PIN_LED,1); busy(LED_BLINK_LOOPS*2);
        PIN_setOutputValue(pinHandle,APP_PIN_LED,0);
        return;
    }
    EasyLink_setFrequency(RF_FREQUENCY_HZ);

    EasyLink_TxPacket tx; memset(&tx,0,sizeof(tx));
    tx.dstAddr[0]=GATEWAY_ADDR; tx.absTime=0; tx.len=14;
    tx.payload[0]=nodeAddr; tx.payload[1]=reason;
    tx.payload[2]=lastBtn1; tx.payload[3]=lastBtn2;
    tx.payload[4]=chipId[0]; tx.payload[5]=chipId[1];
    tx.payload[6]=chipId[2]; tx.payload[7]=chipId[3];
    tx.payload[8]=(uint8_t)(seqNumber>>8); tx.payload[9]=(uint8_t)(seqNumber&0xFF);
    tx.payload[10]=(uint8_t)(g_vbatMv>>8); tx.payload[11]=(uint8_t)(g_vbatMv&0xFF);
    tx.payload[12]=lastBtn3;                 /* 3. kontaktron (DIO9) */
    tx.payload[13]=(uint8_t)g_tempC;         /* temp ukladu, degC ze znakiem */
    seqNumber++;

    /* Anty-kolizja #2: CCA (listen-before-talk) - slucha kanalu i robi backoff,
     * gdy zajety. Czekamy na callback (RF aktywne w tym oknie, bez standby). */
    txDone = false;
    if(EasyLink_transmitCcaAsync(&tx, txDoneCb) == EasyLink_Status_Success){
        while(!txDone){ Power_idleFunc(); }
    } else {
        /* gdyby CCA nie wystartowala (np. brak RNG) - nadaj na slepo */
        EasyLink_transmit(&tx);
    }

    /* Pelne zamkniecie RF - kasuje wewnetrzne zegary RF drivera */
    EasyLink_close();
}

void *mainThread(void *arg0)
{
    pinHandle = PIN_open(&pinState, pinTable);
    if(!pinHandle) while(1);
    PIN_setOutputValue(pinHandle,APP_PIN_LED,0);

    /* Zegary tworzymy PRZED pierwszym blinkiem - blink korzysta z blinkClk. */
    uint32_t usecPerTick = ClockP_getSystemTickPeriod();
    if(usecPerTick==0) usecPerTick=10;
    hbTicks    = (uint32_t)(((uint64_t)HEARTBEAT_MINUTES*60ULL*1000000ULL)/usecPerTick);
    blinkTicks = (uint32_t)(40000u / usecPerTick);   /* ~40 ms swiecenia (tune wg gustu) */
    ticksPerMs = 1000u / usecPerTick; if(ticksPerMs==0) ticksPerMs=1;
    ClockP_Params cp;
    ClockP_Params_init(&cp);
    cp.startFlag=false; cp.period=0;
    hbClock  = ClockP_construct(&hbClockStruct,  hbClockCb,  hbTicks,    &cp);
    blinkClk = ClockP_construct(&blinkClkStruct, blinkClkCb, blinkTicks, &cp);

    /* Tozsamosc + seed PRNG raz na starcie (getIeeeAddr czyta FCFG, nie wymaga RF).
     * Dzieki temu juz pierwszy jitter (power-on TX) jest per-wezel. */
    deriveNodeIdentityFromIeee();
    identityOk = true;

    for(int i=0;i<3;i++){ blinkBothLeds(); busy(1500000U); }

    PIN_registerIntCb(pinHandle, buttonCb);

    lastBtn1 = readBtn(APP_PIN_BTN1);
    lastBtn2 = readBtn(APP_PIN_BTN2);
    lastBtn3 = readBtn(APP_PIN_BTN3);
    armButtonIrq(APP_PIN_BTN1, lastBtn1);
    armButtonIrq(APP_PIN_BTN2, lastBtn2);
    armButtonIrq(APP_PIN_BTN3, lastBtn3);

    /* Power-on TX: pelny cykl RF (po nim RF zamkniete) */
    txWithFullRfCycle(WAKE_POWERON);
    blinkBothLeds();
    restartHeartbeatClock();
    wakeFlag = false;
    heartbeatDue = false;

    while(1){
        while(!wakeFlag){
            Power_idleFunc();
        }
        uintptr_t key = HwiP_disable();
        wakeFlag = false;
        bool hb = heartbeatDue;
        heartbeatDue = false;
        HwiP_restore(key);

        busy(DEBOUNCE_LOOPS);
        uint8_t b1 = readBtn(APP_PIN_BTN1);
        uint8_t b2 = readBtn(APP_PIN_BTN2);
        uint8_t b3 = readBtn(APP_PIN_BTN3);

        uint8_t reason = 0xFF;
        if(b1 != lastBtn1){ lastBtn1=b1; reason=WAKE_BTN1; armButtonIrq(APP_PIN_BTN1,b1); }
        if(b2 != lastBtn2){ lastBtn2=b2; reason=WAKE_BTN2; armButtonIrq(APP_PIN_BTN2,b2); }
        if(b3 != lastBtn3){ lastBtn3=b3; reason=WAKE_BTN3; armButtonIrq(APP_PIN_BTN3,b3); }
        PIN_clrPendInterrupt(pinHandle, APP_PIN_BTN1);
        PIN_clrPendInterrupt(pinHandle, APP_PIN_BTN2);
        PIN_clrPendInterrupt(pinHandle, APP_PIN_BTN3);

        bool didTx = false;
        if(reason != 0xFF){ txWithFullRfCycle(reason); blinkBothLeds(); didTx = true; }
        if(hb){             txWithFullRfCycle(WAKE_HEARTBEAT); blinkBothLeds(); didTx = true; }
        if(didTx) restartHeartbeatClock();
    }
}
