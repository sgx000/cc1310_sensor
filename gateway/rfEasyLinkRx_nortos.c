/*
 * ============================================================================
 *  Gateway dla LAUNCHXL-CC1310 Rev A (NoRTOS)
 * ============================================================================
 *
 *  Co robi:
 *   - Nasluchuje radiowo na 868 MHz (EasyLink, PHY 50 kbps GFSK z smartrf_settings).
 *   - Kazdy odebrany pakiet wysyla jako JSON przez UART (XDS110 backchannel,
 *     115200 8N1).
 *   - Pakiety o len=14 sa traktowane jako "ramki gadgetu" i dekodowane do
 *     czytelnego JSON-a z poszczegolnymi polami. Inne pakiety - surowy hex.
 *   - Utrzymuje TABELE do 30 nadajnikow (kluczowana po node addr). Dla kazdego
 *     liczy stan baterii (>=3.0 V = 100%, <=2.2 V = 0%, liniowo) oraz "online"
 *     (czy ramka przyszla w ostatnich ONLINE_TIMEOUT_MS).
 *   - Co HEARTBEAT_TIMEOUT_MS bez odbioru radiowego wysyla heartbeat.
 *   - Przycisk (DIO14, Board_PIN_BTN2): wlacza OLED (SSD1306 128x32, I2C).
 *     Ekran startowy z wersja FW/SW, potem status urzadzen z tabeli PO KOLEI
 *     (jedno na ekran: online, bateria, switche). Urzadzenie OFFLINE pokazywane
 *     jest w NEGATYWIE. Kazdy ekran trzymany OLED_PAGE_MS, kolejne wcisniecie
 *     = skip do nastepnego; po ostatnim gasi ekran.
 *   - Brak diod LED i sygnalizacji optycznej (caly status idzie na OLED).
 *
 *  Format ramki gadgetu (14 bajtow w polu payload):
 *    [0]      node addr (1 bajt, wyliczony z chip ID gadgetu)
 *    [1]      reason   (0=power-on, 1=BTN1, 2=BTN2, 3=heartbeat, 4=BTN3)
 *    [2]      BTN1 state (0=zwolniony, 1=wcisniety)
 *    [3]      BTN2 state
 *    [4..7]   chip ID  (4 bajty unikalnego IEEE ID gadgetu)
 *    [8][9]   frame sequence (hi/lo)
 *    [10][11] napiecie zasilania w mV (hi/lo)
 *    [12]     BTN3 state
 *    [13]     temperatura (st. C)
 *
 *  Wyjscie JSON dla ramki gadgetu:
 *    {"type":"node","rxseq":N,"rssi":-NN,"node":"XX","reason":"btn1",
 *     "btn1":1,"btn2":0,"btn3":0,"chipId":"XXXXXXXX","fseq":N,
 *     "vbat_mv":3300,"temp":23}
 *
 *  Wazne uwagi techniczne (po dlugim debugowaniu z autorem):
 *   - snprintf z newlib-nano wisi w tym buildzie - cale formatowanie liczb
 *     robimy wlasnymi funkcjami jU/jI/jH2.
 *   - Output skladamy w buforze jbuf i wysylamy JEDNYM UART_write na linie
 *     (uart_write wieloma malymi pisaniami sie przeplata).
 *   - Makefile musi miec poprawiona sciezke bibliotek (thumb/v7-m/nofp
 *     zamiast thumb/v7-m) bo inaczej linker wciaga ARM-owe libki (Cortex-M3
 *     nie umie wykonac ARM mode -> snprintf/memcpy wiesza sie).
 *
 *  Build: SimpleLink CC13x0 SDK 4.20.02.07 + arm-gnu-toolchain 12.3, NoRTOS
 * ============================================================================
 */
#include "smartrf_settings/smartrf_settings.h"
#include "Board.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ti/drivers/pin/PINCC26XX.h>
#include <ti/drivers/PIN.h>
#include <ti/drivers/UART.h>
#include <ti/drivers/I2C.h>
#include <ti/devices/DeviceFamily.h>

#include "easylink/EasyLink.h"
#include "ssd1306.h"

/* === Konfiguracja === */
#define HEARTBEAT_TIMEOUT_MS  60000U     /* 60 s -> wysyl heartbeat gdy cisza */
#define RX_SLICE_MS           1000U      /* dlugosc pojedynczego okna RX */
#define RF_FREQUENCY_HZ       868000000U /* 868 MHz - pasmo EU sub-1GHz */
#define JBUF_SIZE             320        /* bufor na 1 linie JSON */
#define GADGET_FRAME_LEN      14         /* rozpoznawanie ramki gadgetu po dlugosci */

/* Tabela nadajnikow */
#define MAX_NODES             30U        /* ile nadajnikow pamietamy */
#define ONLINE_TIMEOUT_MS     1800000U   /* online jesli ramka < 30 min temu
                                          * (~tyle co heartbeat nadajnika) */
#define BATT_FULL_MV          3000U      /* >= 3.0 V -> 100% */
#define BATT_EMPTY_MV         2200U      /* <= 2.2 V -> 0% */

/* OLED */
#define OLED_PAGE_MS          10000U      /* jak dlugo trzymac jeden ekran */
#define BUSY_PER_MS           8000U      /* ~NOP-ow na 1 ms (kalibracja z petli startowej) */

/* Wersje */
#define FW_VERSION            "1.0.0"    /* wersja firmware */
#define SW_VERSION            "1.0.0"    /* wersja warstwy aplikacyjnej */

/* === Stan globalny === */
static PIN_Handle  pinHandle;
static PIN_State   pinState;
static UART_Handle uart;
static UART_Params uartParams;
static I2C_Handle  i2c = NULL;

/* Pin: przycisk (BTN2, DIO14) - wlacza i przewija ekrany OLED */
PIN_Config pinTable[] = {
    Board_PIN_BTN2 | PIN_INPUT_EN | PIN_PULLUP | PIN_IRQ_NEGEDGE | PIN_HYSTERESIS,
    PIN_TERMINATE
};

static volatile bool btn2Pressed = false;    /* flaga z ISR przycisku (OLED) */
static uint32_t rxCounter = 0;               /* licznik wszystkich odebranych ramek */
static char     jbuf[JBUF_SIZE];             /* bufor budowania linii JSON */
static uint16_t jlen = 0;

/* Czas pochodny od okien RX (przyblizony): rosnie o RX_SLICE_MS na kazde
 * puste okno RX. Wystarczajaco dokladny do wykrywania online/offline. */
static uint32_t g_now_ms = 0;

/* === Tabela nadajnikow === */
typedef struct {
    bool     used;
    uint8_t  node;          /* klucz: node addr z payload[0] */
    uint8_t  chipId[4];
    uint8_t  btn1, btn2, btn3;
    uint16_t vbat_mv;
    uint32_t fseq;
    int8_t   rssi;
    uint32_t lastSeen_ms;   /* g_now_ms przy ostatniej ramce */
} NodeEntry;

static NodeEntry g_nodes[MAX_NODES];

/* === Pomocnicze === */
static void busy(uint32_t n){
    /* Aktywne czekanie - uzywane do mignieciem LED i opoznien OLED. */
    volatile uint32_t k=n; while(k--) __asm__ volatile("nop");
}

static void delayMs(uint32_t ms){ busy(ms * BUSY_PER_MS); }

/* Stan baterii w % wg progow 2.2 V (0%) .. 3.0 V (100%) */
static uint8_t battPct(uint16_t mv){
    if(mv >= BATT_FULL_MV)  return 100;
    if(mv <= BATT_EMPTY_MV) return 0;
    return (uint8_t)(((uint32_t)(mv - BATT_EMPTY_MV) * 100U) / (BATT_FULL_MV - BATT_EMPTY_MV));
}

static bool nodeOnline(const NodeEntry *e){
    return (uint32_t)(g_now_ms - e->lastSeen_ms) < ONLINE_TIMEOUT_MS;
}

static uint8_t nodeCountUsed(void){
    uint8_t n=0;
    for(uint8_t i=0;i<MAX_NODES;i++) if(g_nodes[i].used) n++;
    return n;
}

/* Znajdz wpis po node, albo przydziel wolny. NULL gdy tabela pelna. */
static NodeEntry *findOrAlloc(uint8_t node){
    int freeIdx = -1;
    for(int i=0;i<(int)MAX_NODES;i++){
        if(g_nodes[i].used && g_nodes[i].node == node) return &g_nodes[i];
        if(freeIdx < 0 && !g_nodes[i].used) freeIdx = i;
    }
    if(freeIdx >= 0) return &g_nodes[freeIdx];
    return NULL;
}

/* Aktualizacja tabeli z odebranej (lub dummy) ramki gadgetu. */
static void updateTable(const EasyLink_RxPacket *rx){
    if(rx->len != GADGET_FRAME_LEN) return;     /* tabela tylko dla ramek gadgetu */
    NodeEntry *e = findOrAlloc(rx->payload[0]);
    if(!e) return;                              /* tabela pelna - ignoruj nowy node */
    e->used    = true;
    e->node    = rx->payload[0];
    e->btn1    = rx->payload[2];
    e->btn2    = rx->payload[3];
    e->chipId[0]=rx->payload[4]; e->chipId[1]=rx->payload[5];
    e->chipId[2]=rx->payload[6]; e->chipId[3]=rx->payload[7];
    e->fseq    = ((uint32_t)rx->payload[8] << 8) | rx->payload[9];
    e->vbat_mv = ((uint16_t)rx->payload[10] << 8) | rx->payload[11];
    e->btn3    = rx->payload[12];
    e->rssi    = rx->rssi;
    e->lastSeen_ms = g_now_ms;
}

/* === Builder linii JSON do bufora jbuf ===
 * Nie uzywamy snprintf bo z newlib-nano wisi w tym buildzie. */
static void jReset(void){ jlen=0; jbuf[0]='\0'; }
static void jC(char c){ if(jlen<JBUF_SIZE-1){ jbuf[jlen++]=c; jbuf[jlen]='\0'; } }
static void jS(const char *s){ while(*s) jC(*s++); }

/* uint -> dziesietnie, bez wiodacych zer */
static void jU(uint32_t v){
    char t[12]; int i=11; t[i--]='\0';
    if(v==0) t[i--]='0';
    while(v){ t[i--]='0'+(v%10); v/=10; }
    jS(&t[i+1]);
}

/* int -> dziesietnie ze znakiem (dla rssi ktore jest ujemne) */
static void jI(int32_t v){
    if(v<0){ jC('-'); jU((uint32_t)(-v)); }
    else    jU((uint32_t)v);
}

/* uint8 -> dwa znaki hex bez "0x" (np. "9D") */
static void jH2(uint8_t b){
    static const char H[]="0123456789ABCDEF";
    jC(H[(b>>4)&0xF]); jC(H[b&0xF]);
}

/* Wyslij zbudowana linie - JEDNYM UART_write (male pisania by sie przeplatały) */
static void jFlush(void){
    if(uart && jlen){
        UART_write(uart,(void*)jbuf,jlen);
    }
    jReset();
}

/* Skrot: gotowa statyczna linia */
static void line(const char *s){ jReset(); jS(s); jFlush(); }

/* Czytelna nazwa powodu w ramce gadgetu */
static const char *reasonStr(uint8_t r){
    switch(r){
        case 0: return "poweron";
        case 1: return "btn1";
        case 2: return "btn2";
        case 3: return "heartbeat";
        case 4: return "btn3";
        default: return "unknown";
    }
}

/* === Format jednej ramki na wyjscie === */
static void sendDataPacket(EasyLink_RxPacket *rx){
    jReset();
    if(rx->len == GADGET_FRAME_LEN){
        /* === Ramka gadgetu - pelny JSON z dekodowanymi polami === */
        uint8_t  node   = rx->payload[0];
        uint8_t  reason = rx->payload[1];
        uint8_t  btn1   = rx->payload[2];
        uint8_t  btn2   = rx->payload[3];
        uint32_t fseq   = ((uint32_t)rx->payload[8] << 8) | rx->payload[9];
        uint32_t vbat   = ((uint32_t)rx->payload[10] << 8) | rx->payload[11];
        uint8_t  btn3   = rx->payload[12];
        uint8_t  temp   = rx->payload[13];

        jS("{\"type\":\"node\"");
        jS(",\"rxseq\":"); jU(rxCounter);
        jS(",\"rssi\":"); jI((int32_t)rx->rssi);
        jS(",\"node\":\""); jH2(node); jS("\"");
        jS(",\"reason\":\""); jS(reasonStr(reason)); jS("\"");
        jS(",\"btn1\":"); jU(btn1);
        jS(",\"btn2\":"); jU(btn2);
        jS(",\"btn3\":"); jU(btn3);
        jS(",\"chipId\":\"");
        jH2(rx->payload[4]); jH2(rx->payload[5]);
        jH2(rx->payload[6]); jH2(rx->payload[7]);
        jS("\"");
        jS(",\"fseq\":"); jU(fseq);
        jS(",\"vbat_mv\":"); jU(vbat);
        jS(",\"temp\":"); jU(temp);
        jS("}\r\n");
    } else {
        /* === Nieznana ramka - surowy payload hex w polu "raw" === */
        jS("{\"type\":\"data\"");
        jS(",\"rxseq\":"); jU(rxCounter);
        jS(",\"rssi\":"); jI((int32_t)rx->rssi);
        jS(",\"addr\":\""); jH2(rx->dstAddr[0]); jS("\"");
        jS(",\"len\":"); jU(rx->len);
        jS(",\"raw\":\"");
        for(uint16_t i=0;i<rx->len && jlen < JBUF_SIZE-12;i++) jH2(rx->payload[i]);
        jS("\"}\r\n");
    }
    jFlush();
}

/* Heartbeat: gateway dziala, ale nie odbiera nic z radia */
static void sendHeartbeat(const char *cause, uint32_t loopCount){
    jReset();
    jS("{\"type\":\"heartbeat\",\"status\":\"no data received\",\"cause\":\"");
    jS(cause); jS("\",\"loop\":"); jU(loopCount); jS("}\r\n");
    jFlush();
}

/* === BTN2: OLED ===
 * Pokazuje urzadzenia z tabeli po kolei (jedno na ekran), potem gasi panel. */

/* Drobne formatowanie do bufora znakow (zwraca koniec). */
static char *appS(char *d, const char *s){ while(*s) *d++ = *s++; return d; }
static char *appU(char *d, uint32_t v){
    char t[12]; int i=11; t[i--]='\0';
    if(v==0) t[i--]='0';
    while(v){ t[i--]='0'+(v%10); v/=10; }
    return appS(d, &t[i+1]);
}
static char *appI(char *d, int32_t v){
    if(v<0){ *d++='-'; return appU(d,(uint32_t)(-v)); }
    return appU(d,(uint32_t)v);
}
static char *appH2(char *d, uint8_t b){
    static const char H[]="0123456789ABCDEF";
    *d++ = H[(b>>4)&0xF]; *d++ = H[b&0xF]; return d;
}

static void oledRenderNode(const NodeEntry *e, uint8_t idx1, uint8_t total){
    char ln[24]; char *p;
    ssd1306_clear();

    /* Wiersz 0: "Node DD  3/12" */
    p = ln; p = appS(p,"Node "); p = appH2(p, e->node); p = appS(p,"  ");
    p = appU(p, idx1); *p++='/'; p = appU(p, total); *p='\0';
    ssd1306_draw_string(0,0,ln);

    /* Wiersz 1: "ONLINE -42dBm" / "OFFLINE -42dBm" */
    p = ln;
    p = appS(p, nodeOnline(e) ? "ONLINE " : "OFFLINE ");
    p = appI(p, e->rssi); p = appS(p,"dBm"); *p='\0';
    ssd1306_draw_string(0,1,ln);

    /* Wiersz 2: "Bat 94% 2950mV" */
    p = ln; p = appS(p,"Bat "); p = appU(p, battPct(e->vbat_mv)); p = appS(p,"% ");
    p = appU(p, e->vbat_mv); p = appS(p,"mV"); *p='\0';
    ssd1306_draw_string(0,2,ln);

    /* Wiersz 3: "SW 1:1 2:0 3:1" */
    p = ln; p = appS(p,"SW 1:"); p = appU(p, e->btn1?1:0);
    p = appS(p," 2:"); p = appU(p, e->btn2?1:0);
    p = appS(p," 3:"); p = appU(p, e->btn3?1:0); *p='\0';
    ssd1306_draw_string(0,3,ln);

    /* Offline -> caly ekran w NEGATYWIE (lepsza widocznosc) */
    ssd1306_set_invert(!nodeOnline(e));
}

/* Czeka do `ms` albo do wcisniecia BTN2 (skip do nastepnego ekranu).
 * Zwraca true gdy przerwano guzikiem. Granulacja 50 ms. */
static bool oledWaitOrButton(uint32_t ms){
    for(uint32_t t = 0; t < ms; t += 50U){
        if(btn2Pressed){ btn2Pressed = false; return true; }
        delayMs(50U);
    }
    return false;
}

/* Ekran startowy: wersja sw/fw + liczba nadajnikow w tabeli */
static void oledRenderVersion(void){
    char ln[24]; char *p;
    ssd1306_clear();
    ssd1306_draw_string(0,0,"CC1310 Gateway");
    ssd1306_draw_string(0,1,"FW " FW_VERSION);
    ssd1306_draw_string(0,2,"SW " SW_VERSION);
    p = ln; p = appS(p,"Nodes: "); p = appU(p, nodeCountUsed()); *p='\0';
    ssd1306_draw_string(0,3,ln);
    ssd1306_set_invert(false);   /* ekran wersji zawsze normalnie */
}

static void oledShowAll(void){
    if(!ssd1306_present()){
        line("{\"type\":\"warn\",\"info\":\"OLED not present\"}\r\n");
        return;
    }
    uint8_t total = nodeCountUsed();
    ssd1306_display_on();

    /* Ekran startowy z wersja sw/fw */
    oledRenderVersion();
    ssd1306_flush();
    oledWaitOrButton(OLED_PAGE_MS);

    if(total == 0){
        ssd1306_clear();
        ssd1306_draw_string(0,0,"Brak urzadzen");
        ssd1306_draw_string(0,1,"w tabeli");
        ssd1306_flush();
        oledWaitOrButton(OLED_PAGE_MS);
    } else {
        uint8_t shown = 0;
        for(uint8_t i=0;i<MAX_NODES;i++){
            if(!g_nodes[i].used) continue;
            shown++;
            oledRenderNode(&g_nodes[i], shown, total);
            ssd1306_flush();
            oledWaitOrButton(OLED_PAGE_MS);
        }
    }

    /* Po pokazaniu wszystkich - normalny tryb, czysty ekran, panel off. */
    ssd1306_set_invert(false);
    ssd1306_clear();
    ssd1306_flush();
    ssd1306_display_off();
    btn2Pressed = false;   /* ignoruj przypadkowe wcisniecia podczas wygaszania */
}

/* ISR przyciskow - tylko ustawiaja flagi (obsluga w petli glownej). */
static void buttonCb(PIN_Handle h, PIN_Id pinId){
    (void)h;
    if(pinId == Board_PIN_BTN2) btn2Pressed = true;
}

/* === Watek glowny (NoRTOS - wywolywany raz z main_nortos.c) === */
void *mainThread(void *arg0)
{
    (void)arg0;
    /* PIN driver: przycisk (BTN2, DIO14) z przerwaniem na zbocze opadajace */
    pinHandle = PIN_open(&pinState, pinTable);
    if(!pinHandle) while(1);

    /* UART backchannel: 115200 8N1, dostepny jako wirtualny COM po USB
     * (XDS110 Class Application/User UART) */
    UART_init();
    UART_Params_init(&uartParams);
    uartParams.writeDataMode  = UART_DATA_BINARY;
    uartParams.readDataMode   = UART_DATA_BINARY;
    uartParams.readReturnMode = UART_RETURN_FULL;
    uartParams.readEcho       = UART_ECHO_OFF;
    uartParams.writeMode      = UART_MODE_BLOCKING;
    uartParams.readMode       = UART_MODE_BLOCKING;
    uartParams.baudRate       = 115200;
    uart = UART_open(Board_UART0,&uartParams);
    if(!uart){
        /* UART nie wystartowal - brak kanalu diagnostycznego, zatrzymaj sie */
        while(1);
    }

    line("{\"type\":\"boot\",\"info\":\"CC1310 gateway started\",\"fw\":\"" FW_VERSION "\",\"sw\":\"" SW_VERSION "\"}\r\n");

    /* I2C + OLED SSD1306 (DIO4 SCL / DIO5 SDA). Panel zostaje wylaczony,
     * wlaczy go dopiero BTN2. Brak panelu nie jest bledem krytycznym. */
    I2C_init();
    I2C_Params i2cParams;
    I2C_Params_init(&i2cParams);
    i2cParams.bitRate = I2C_400kHz;
    i2c = I2C_open(Board_I2C0, &i2cParams);
    bool oledOk = (i2c != NULL) && ssd1306_init(i2c);
    line(oledOk ? "{\"type\":\"info\",\"oled\":\"ssd1306 ready\"}\r\n"
                : "{\"type\":\"warn\",\"oled\":\"ssd1306 not found\"}\r\n");

    /* Rejestracja callbacku przyciskow po otwarciu pinow */
    PIN_registerIntCb(pinHandle, buttonCb);

    /* Init radia: PHY z smartrf_settings.c (Custom = 50 kbps GFSK), 868 MHz */
    EasyLink_Params elp;
    EasyLink_Params_init(&elp);
    elp.ui32ModType = EasyLink_Phy_Custom;

    EasyLink_Status initStatus = EasyLink_init(&elp);
    bool radioOk = (initStatus == EasyLink_Status_Success);

    if(radioOk){
        if(EasyLink_setFrequency(RF_FREQUENCY_HZ) != EasyLink_Status_Success){
            line("{\"type\":\"warn\",\"info\":\"setFrequency failed\"}\r\n");
        }
        line("{\"type\":\"ready\",\"freq_hz\":868000000,\"phy\":\"custom_50kbps_gfsk\"}\r\n");
    } else {
        jReset();
        jS("{\"type\":\"error\",\"info\":\"EasyLink_init failed\",\"status\":");
        jI((int32_t)initStatus); jS("}\r\n");
        jFlush();
    }

    /* Petla glowna: cykl RX o dlugosci RX_SLICE_MS, akumulacja licznika
     * "ciszy" do wyzwolenia heartbeat */
    uint32_t silentSlices = 0;
    uint32_t loopCount = 0;

    while(1){
        loopCount++;

        /* Przycisk (BTN2): pokaz tabele na OLED, potem zgas ekran */
        if(btn2Pressed){
            btn2Pressed = false;
            oledShowAll();
            silentSlices = 0;
        }

        if(radioOk){
            EasyLink_RxPacket rxPacket;
            memset(&rxPacket,0,sizeof(rxPacket));
            rxPacket.absTime   = 0;
            rxPacket.rxTimeout = EasyLink_ms_To_RadioTime(RX_SLICE_MS);

            /* Synchroniczny odbior - blokuje az pakiet przyjdzie albo timeout */
            EasyLink_Status st = EasyLink_receive(&rxPacket);

            if(st == EasyLink_Status_Success){
                rxCounter++;
                updateTable(&rxPacket);
                sendDataPacket(&rxPacket);
                silentSlices = 0;
            }
            else if(st == EasyLink_Status_Rx_Timeout){
                g_now_ms += RX_SLICE_MS;      /* czas plynie tylko w ciszy */
                silentSlices++;
                if(silentSlices * RX_SLICE_MS >= HEARTBEAT_TIMEOUT_MS){
                    sendHeartbeat("timeout", loopCount);
                    silentSlices = 0;
                }
            }
            else if(st == EasyLink_Status_Aborted){
                /* moglo zostac przerwane np. przez RF driver - kolejna iteracja */
            }
            else {
                /* Inny blad RX - zaloguj i pauza zeby nie zasypac UART */
                jReset();
                jS("{\"type\":\"error\",\"info\":\"EasyLink_receive\",\"code\":");
                jI((int32_t)st); jS("}\r\n");
                jFlush();
                busy(2000000U);
            }
        } else {
            /* Tryb bez radia - tylko heartbeat z busy delay (awaryjny) */
            busy(16000000U);
            g_now_ms += RX_SLICE_MS;
            silentSlices++;
            if(silentSlices >= 30){
                sendHeartbeat("timeout-noradio", loopCount);
                silentSlices = 0;
            }
        }
    }
}
