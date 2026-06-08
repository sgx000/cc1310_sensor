/*
 * ============================================================================
 *  ssd1306.h - minimalny sterownik OLED SSD1306 128x32 po I2C (CC1310)
 * ============================================================================
 *
 *  Podlaczenie (LAUNCHXL-CC1310, Board_I2C0):
 *    SCL  -> DIO4   (CC1310_LAUNCHXL_I2C0_SCL0)
 *    SDA  -> DIO5   (CC1310_LAUNCHXL_I2C0_SDA0)
 *    VCC  -> 3V3
 *    GND  -> GND
 *  Adres I2C panelu: 0x3C (typowy dla modulow 128x32; rzadziej 0x3D).
 *
 *  Font: wbudowany 5x7 (6 px z odstepem) -> 21 znakow x 4 wiersze.
 *  Framebuffer trzymany w RAM (512 B), wypychany przez ssd1306_flush().
 *
 *  Do testow na hoscie skompiluj z -DSSD1306_HOST_TEST (I2C jest wtedy
 *  zaslepione, a logika fontu/framebuffera dziala jak na docelowym ukladzie).
 * ============================================================================
 */
#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(SSD1306_HOST_TEST)
typedef void *I2C_Handle;            /* zaslepka na potrzeby testow na hoscie */
#else
#include <ti/drivers/I2C.h>
#endif

#define SSD1306_WIDTH      128
#define SSD1306_HEIGHT     32
#define SSD1306_PAGES      (SSD1306_HEIGHT / 8)   /* 4 strony po 8 px */
#define SSD1306_FB_SIZE    (SSD1306_WIDTH * SSD1306_PAGES)
#define SSD1306_I2C_ADDR   0x3C
#define SSD1306_CHAR_W     6                       /* 5 px glif + 1 px odstep */
#define SSD1306_COLS       (SSD1306_WIDTH / SSD1306_CHAR_W)  /* 21 znakow */

/* Inicjalizacja panelu. Zwraca false jesli panel nie potwierdza na I2C
 * (np. nie podlaczony) - wtedy caller moze pominac obsluge OLED. */
bool ssd1306_init(I2C_Handle handle);

/* Czy panel zostal wykryty przy init. */
bool ssd1306_present(void);

/* Wlaczenie / wylaczenie matrycy (komenda 0xAF / 0xAE). Sterownik pamieta
 * stan, wiec mozna pytac ssd1306_is_on(). */
void ssd1306_display_on(void);
void ssd1306_display_off(void);
bool ssd1306_is_on(void);

/* Negatyw (inwersja) calego panelu - komenda 0xA7 (inv) / 0xA6 (normal).
 * Przydatne np. do wyroznienia urzadzenia offline. */
void ssd1306_set_invert(bool inv);
bool ssd1306_is_inverted(void);

/* Czyszczenie framebuffera (nie wysyla - wywolaj potem flush). */
void ssd1306_clear(void);

/* Rysowanie do framebuffera. col w pikselach 0..127, page (wiersz) 0..3. */
void ssd1306_draw_char(uint8_t col, uint8_t page, char c);
void ssd1306_draw_string(uint8_t col, uint8_t page, const char *s);

/* Wypchniecie framebuffera na panel. Zwraca false przy bledzie I2C. */
bool ssd1306_flush(void);

/* Dostep do framebuffera (debug / testy na hoscie). */
const uint8_t *ssd1306_framebuffer(void);

#endif /* SSD1306_H */
