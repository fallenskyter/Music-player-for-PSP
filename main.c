/* ======================================================================
 *  SOFT//CLUB  -  MP3 player for PSP (custom firmware)
 *
 *  - MP3 files from  ms0:/MUSIC/  (or ef0:/MUSIC/ on PSP Go)
 *  - audio runs in its own thread, so the UI can never make it stutter
 *  - the UI is software-drawn, the background is pre-rendered once per theme
 *    and the screen is redrawn only when something actually changed
 *  - every track fully releases its decoder / audio channel / file,
 *    so switching tracks any number of times works
 *
 *  Controls
 *    UP / DOWN      move in the list
 *    LEFT / RIGHT   volume
 *    CROSS (X)      play selected track / pause / resume
 *    SQUARE         stop
 *    L / R          previous / next track
 *    TRIANGLE       next theme
 * ====================================================================== */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspmp3.h>
#include <psputility.h>
#include <psppower.h>
#include <psputils.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

PSP_MODULE_INFO("SoftClub", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);

/* ---------------------------------------------------------------- setup */
#define SW 480
#define SH 272
#define SB 512                     /* framebuffer stride (pixels) */
#define FRAME_BYTES (SB * SH * 4)

#define MAX_TRACKS   256
#define LIST_ROWS    9
#define N_THEMES     5

#define MP3_BUF_SIZE (16 * 1024)
#define PCM_BUF_SIZE (16 * 1024)
#define OUT_BLOCK    1024          /* samples per audio output block (multiple of 64) */

enum { ST_STOP = 0, ST_LOAD, ST_PLAY, ST_PAUSE, ST_ERROR };
enum { PR_ENDED = 0, PR_REQUEST, PR_ERROR, PR_QUIT };

/* Bitmap fonts (public-domain X11 misc-fixed style), glyphs 32..126, MSB = left pixel */
static const unsigned char FONT_S[95][10] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x10,0x10,0x10,0x10,0x10,0x00,0x10,0x00,0x00}, /* ! */
    {0x00,0x28,0x28,0x28,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x00,0x28,0x28,0x7C,0x28,0x7C,0x28,0x28,0x00,0x00}, /* # */
    {0x00,0x10,0x38,0x50,0x38,0x14,0x38,0x10,0x00,0x00}, /* $ */
    {0x00,0x24,0x54,0x28,0x10,0x28,0x54,0x48,0x00,0x00}, /* % */
    {0x00,0x20,0x50,0x50,0x20,0x54,0x48,0x34,0x00,0x00}, /* & */
    {0x00,0x18,0x10,0x20,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x00,0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00,0x00}, /* ( */
    {0x00,0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00,0x00}, /* ) */
    {0x00,0x00,0x44,0x28,0x7C,0x28,0x44,0x00,0x00,0x00}, /* ? */
    {0x00,0x00,0x10,0x10,0x7C,0x10,0x10,0x00,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x10,0x20,0x00}, /* , */
    {0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x38,0x10,0x00}, /* . */
    {0x00,0x04,0x04,0x08,0x10,0x20,0x40,0x40,0x00,0x00}, /* ? */
    {0x00,0x10,0x28,0x44,0x44,0x44,0x28,0x10,0x00,0x00}, /* 0 */
    {0x00,0x10,0x30,0x50,0x10,0x10,0x10,0x7C,0x00,0x00}, /* 1 */
    {0x00,0x38,0x44,0x04,0x18,0x20,0x40,0x7C,0x00,0x00}, /* 2 */
    {0x00,0x7C,0x04,0x08,0x18,0x04,0x44,0x38,0x00,0x00}, /* 3 */
    {0x00,0x08,0x18,0x28,0x48,0x7C,0x08,0x08,0x00,0x00}, /* 4 */
    {0x00,0x7C,0x40,0x58,0x64,0x04,0x44,0x38,0x00,0x00}, /* 5 */
    {0x00,0x18,0x20,0x40,0x58,0x64,0x44,0x38,0x00,0x00}, /* 6 */
    {0x00,0x7C,0x04,0x08,0x08,0x10,0x20,0x20,0x00,0x00}, /* 7 */
    {0x00,0x38,0x44,0x44,0x38,0x44,0x44,0x38,0x00,0x00}, /* 8 */
    {0x00,0x38,0x44,0x4C,0x34,0x04,0x08,0x30,0x00,0x00}, /* 9 */
    {0x00,0x00,0x10,0x38,0x10,0x00,0x10,0x38,0x10,0x00}, /* : */
    {0x00,0x00,0x10,0x38,0x10,0x00,0x18,0x10,0x20,0x00}, /* ; */
    {0x00,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00,0x00}, /* < */
    {0x00,0x00,0x00,0x7C,0x00,0x7C,0x00,0x00,0x00,0x00}, /* = */
    {0x00,0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x00,0x00}, /* > */
    {0x00,0x38,0x44,0x08,0x10,0x10,0x00,0x10,0x00,0x00}, /* ? */
    {0x00,0x38,0x44,0x4C,0x54,0x58,0x40,0x38,0x00,0x00}, /* @ */
    {0x00,0x10,0x28,0x44,0x44,0x7C,0x44,0x44,0x00,0x00}, /* A */
    {0x00,0x78,0x24,0x24,0x38,0x24,0x24,0x78,0x00,0x00}, /* B */
    {0x00,0x38,0x44,0x40,0x40,0x40,0x44,0x38,0x00,0x00}, /* C */
    {0x00,0x78,0x24,0x24,0x24,0x24,0x24,0x78,0x00,0x00}, /* D */
    {0x00,0x7C,0x40,0x40,0x78,0x40,0x40,0x7C,0x00,0x00}, /* E */
    {0x00,0x7C,0x40,0x40,0x78,0x40,0x40,0x40,0x00,0x00}, /* F */
    {0x00,0x38,0x44,0x40,0x40,0x4C,0x44,0x38,0x00,0x00}, /* G */
    {0x00,0x44,0x44,0x44,0x7C,0x44,0x44,0x44,0x00,0x00}, /* H */
    {0x00,0x38,0x10,0x10,0x10,0x10,0x10,0x38,0x00,0x00}, /* I */
    {0x00,0x1C,0x04,0x04,0x04,0x04,0x44,0x38,0x00,0x00}, /* J */
    {0x00,0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00,0x00}, /* K */
    {0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x7C,0x00,0x00}, /* L */
    {0x00,0x44,0x44,0x6C,0x54,0x44,0x44,0x44,0x00,0x00}, /* M */
    {0x00,0x44,0x44,0x64,0x54,0x4C,0x44,0x44,0x00,0x00}, /* N */
    {0x00,0x38,0x44,0x44,0x44,0x44,0x44,0x38,0x00,0x00}, /* O */
    {0x00,0x78,0x44,0x44,0x78,0x40,0x40,0x40,0x00,0x00}, /* P */
    {0x00,0x38,0x44,0x44,0x44,0x44,0x54,0x38,0x04,0x00}, /* Q */
    {0x00,0x78,0x44,0x44,0x78,0x50,0x48,0x44,0x00,0x00}, /* R */
    {0x00,0x38,0x44,0x40,0x38,0x04,0x44,0x38,0x00,0x00}, /* S */
    {0x00,0x7C,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, /* T */
    {0x00,0x44,0x44,0x44,0x44,0x44,0x44,0x38,0x00,0x00}, /* U */
    {0x00,0x44,0x44,0x44,0x28,0x28,0x28,0x10,0x00,0x00}, /* V */
    {0x00,0x44,0x44,0x44,0x54,0x54,0x6C,0x44,0x00,0x00}, /* W */
    {0x00,0x44,0x44,0x28,0x10,0x28,0x44,0x44,0x00,0x00}, /* X */
    {0x00,0x44,0x44,0x28,0x10,0x10,0x10,0x10,0x00,0x00}, /* Y */
    {0x00,0x7C,0x04,0x08,0x10,0x20,0x40,0x7C,0x00,0x00}, /* Z */
    {0x00,0x38,0x20,0x20,0x20,0x20,0x20,0x38,0x00,0x00}, /* [ */
    {0x00,0x40,0x40,0x20,0x10,0x08,0x04,0x04,0x00,0x00}, /* ? */
    {0x00,0x38,0x08,0x08,0x08,0x08,0x08,0x38,0x00,0x00}, /* ] */
    {0x00,0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x00}, /* _ */
    {0x00,0x30,0x10,0x08,0x00,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00,0x00}, /* a */
    {0x00,0x40,0x40,0x58,0x64,0x44,0x64,0x58,0x00,0x00}, /* b */
    {0x00,0x00,0x00,0x38,0x44,0x40,0x44,0x38,0x00,0x00}, /* c */
    {0x00,0x04,0x04,0x34,0x4C,0x44,0x4C,0x34,0x00,0x00}, /* d */
    {0x00,0x00,0x00,0x38,0x44,0x7C,0x40,0x38,0x00,0x00}, /* e */
    {0x00,0x18,0x24,0x20,0x78,0x20,0x20,0x20,0x00,0x00}, /* f */
    {0x00,0x00,0x00,0x34,0x48,0x30,0x40,0x38,0x44,0x38}, /* g */
    {0x00,0x40,0x40,0x58,0x64,0x44,0x44,0x44,0x00,0x00}, /* h */
    {0x00,0x10,0x00,0x30,0x10,0x10,0x10,0x38,0x00,0x00}, /* i */
    {0x00,0x08,0x00,0x18,0x08,0x08,0x08,0x48,0x48,0x30}, /* j */
    {0x00,0x40,0x40,0x44,0x48,0x70,0x48,0x44,0x00,0x00}, /* k */
    {0x00,0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00,0x00}, /* l */
    {0x00,0x00,0x00,0x68,0x54,0x54,0x54,0x44,0x00,0x00}, /* m */
    {0x00,0x00,0x00,0x58,0x64,0x44,0x44,0x44,0x00,0x00}, /* n */
    {0x00,0x00,0x00,0x38,0x44,0x44,0x44,0x38,0x00,0x00}, /* o */
    {0x00,0x00,0x00,0x58,0x64,0x64,0x58,0x40,0x40,0x40}, /* p */
    {0x00,0x00,0x00,0x34,0x4C,0x4C,0x34,0x04,0x04,0x04}, /* q */
    {0x00,0x00,0x00,0x58,0x64,0x40,0x40,0x40,0x00,0x00}, /* r */
    {0x00,0x00,0x00,0x38,0x40,0x38,0x04,0x78,0x00,0x00}, /* s */
    {0x00,0x20,0x20,0x78,0x20,0x20,0x24,0x18,0x00,0x00}, /* t */
    {0x00,0x00,0x00,0x44,0x44,0x44,0x4C,0x34,0x00,0x00}, /* u */
    {0x00,0x00,0x00,0x44,0x44,0x28,0x28,0x10,0x00,0x00}, /* v */
    {0x00,0x00,0x00,0x44,0x44,0x54,0x54,0x28,0x00,0x00}, /* w */
    {0x00,0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x00,0x00}, /* x */
    {0x00,0x00,0x00,0x44,0x44,0x4C,0x34,0x04,0x44,0x38}, /* y */
    {0x00,0x00,0x00,0x7C,0x08,0x10,0x20,0x7C,0x00,0x00}, /* z */
    {0x00,0x0C,0x10,0x08,0x30,0x08,0x10,0x0C,0x00,0x00}, /* { */
    {0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00}, /* | */
    {0x00,0x60,0x10,0x20,0x18,0x20,0x10,0x60,0x00,0x00}, /* } */
    {0x00,0x24,0x54,0x48,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};
static const unsigned char FONT_B[95][13] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x00,0x30,0x00,0x00}, /* ! */
    {0x00,0x00,0xD8,0xD8,0xD8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x00,0x00,0x00,0x78,0x78,0xFC,0x78,0xFC,0x78,0x78,0x00,0x00,0x00}, /* # */
    {0x00,0x00,0x00,0x30,0x78,0xF0,0x78,0x3C,0x78,0x30,0x00,0x00,0x00}, /* $ */
    {0x00,0x00,0x6C,0xFC,0x78,0x18,0x30,0x60,0x78,0xFC,0xD8,0x00,0x00}, /* % */
    {0x00,0x00,0x00,0x00,0x60,0xF0,0xF0,0x60,0xFC,0xD8,0x7C,0x00,0x00}, /* & */
    {0x00,0x00,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x00,0x18,0x30,0x30,0x60,0x60,0x60,0x60,0x60,0x30,0x30,0x18,0x00}, /* ( */
    {0x00,0x60,0x30,0x30,0x18,0x18,0x18,0x18,0x18,0x30,0x30,0x60,0x00}, /* ) */
    {0x00,0x00,0x00,0x00,0xCC,0x78,0xFC,0x78,0xCC,0x00,0x00,0x00,0x00}, /* ? */
    {0x00,0x00,0x00,0x00,0x30,0x30,0xFC,0x30,0x30,0x00,0x00,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x30,0x60,0x00}, /* , */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0x00,0x00,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x78,0x30,0x00}, /* . */
    {0x00,0x00,0x0C,0x0C,0x18,0x18,0x30,0x60,0x60,0xC0,0xC0,0x00,0x00}, /* ? */
    {0x00,0x00,0x30,0x78,0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00,0x00}, /* 0 */
    {0x00,0x00,0x30,0x70,0xF0,0x30,0x30,0x30,0x30,0x30,0xFC,0x00,0x00}, /* 1 */
    {0x00,0x00,0x78,0xCC,0xCC,0x0C,0x18,0x30,0x60,0xC0,0xFC,0x00,0x00}, /* 2 */
    {0x00,0x00,0xFC,0x0C,0x18,0x30,0x78,0x0C,0x0C,0xCC,0x78,0x00,0x00}, /* 3 */
    {0x00,0x00,0x18,0x18,0x38,0x78,0x58,0xD8,0xFC,0x18,0x18,0x00,0x00}, /* 4 */
    {0x00,0x00,0xFC,0xC0,0xC0,0xF8,0xEC,0x0C,0x0C,0xCC,0x78,0x00,0x00}, /* 5 */
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xF8,0xEC,0xCC,0xCC,0x78,0x00,0x00}, /* 6 */
    {0x00,0x00,0xFC,0x0C,0x18,0x18,0x30,0x30,0x60,0x60,0x60,0x00,0x00}, /* 7 */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0xCC,0xCC,0xCC,0x78,0x00,0x00}, /* 8 */
    {0x00,0x00,0x78,0xCC,0xCC,0xDC,0x7C,0x0C,0x0C,0x18,0x70,0x00,0x00}, /* 9 */
    {0x00,0x00,0x00,0x30,0x78,0x30,0x00,0x00,0x30,0x78,0x30,0x00,0x00}, /* : */
    {0x00,0x00,0x00,0x00,0x30,0x78,0x30,0x00,0x00,0x38,0x30,0x60,0x00}, /* ; */
    {0x00,0x00,0x0C,0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x0C,0x00,0x00}, /* < */
    {0x00,0x00,0x00,0x00,0x00,0xFC,0x00,0x00,0xFC,0x00,0x00,0x00,0x00}, /* = */
    {0x00,0x00,0xC0,0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0xC0,0x00,0x00}, /* > */
    {0x00,0x00,0x78,0xCC,0xCC,0x0C,0x18,0x30,0x30,0x00,0x30,0x00,0x00}, /* ? */
    {0x00,0x00,0x78,0xCC,0xCC,0xDC,0xFC,0xFC,0xF8,0xC0,0x7C,0x00,0x00}, /* @ */
    {0x00,0x00,0x30,0x78,0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00,0x00}, /* A */
    {0x00,0x00,0xF8,0x6C,0x6C,0x6C,0x78,0x6C,0x6C,0x6C,0xF8,0x00,0x00}, /* B */
    {0x00,0x00,0x78,0xCC,0xC0,0xC0,0xC0,0xC0,0xC0,0xCC,0x78,0x00,0x00}, /* C */
    {0x00,0x00,0xF8,0x6C,0x6C,0x6C,0x6C,0x6C,0x6C,0x6C,0xF8,0x00,0x00}, /* D */
    {0x00,0x00,0xFC,0xC0,0xC0,0xC0,0xF8,0xC0,0xC0,0xC0,0xFC,0x00,0x00}, /* E */
    {0x00,0x00,0xFC,0xC0,0xC0,0xC0,0xF8,0xC0,0xC0,0xC0,0xC0,0x00,0x00}, /* F */
    {0x00,0x00,0x78,0xCC,0xC0,0xC0,0xC0,0xDC,0xCC,0xCC,0x78,0x00,0x00}, /* G */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0xCC,0x00,0x00}, /* H */
    {0x00,0x00,0x78,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x78,0x00,0x00}, /* I */
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0x78,0x00,0x00}, /* J */
    {0x00,0x00,0xCC,0xCC,0xD8,0xF0,0xE0,0xF0,0xD8,0xCC,0xCC,0x00,0x00}, /* K */
    {0x00,0x00,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFC,0x00,0x00}, /* L */
    {0x00,0x00,0xCC,0xCC,0xFC,0xFC,0xFC,0xCC,0xCC,0xCC,0xCC,0x00,0x00}, /* M */
    {0x00,0x00,0xCC,0xEC,0xEC,0xFC,0xFC,0xDC,0xDC,0xCC,0xCC,0x00,0x00}, /* N */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00}, /* O */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xF8,0xC0,0xC0,0xC0,0xC0,0x00,0x00}, /* P */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x78,0x0C,0x00}, /* Q */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xF8,0xF0,0xD8,0xCC,0xCC,0x00,0x00}, /* R */
    {0x00,0x00,0x78,0xCC,0xC0,0xC0,0x78,0x0C,0x0C,0xCC,0x78,0x00,0x00}, /* S */
    {0x00,0x00,0xFC,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x00,0x00}, /* T */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00}, /* U */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x78,0x78,0x30,0x30,0x00,0x00}, /* V */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xFC,0xFC,0xFC,0xFC,0xCC,0x00,0x00}, /* W */
    {0x00,0x00,0xCC,0xCC,0x78,0x78,0x30,0x78,0x78,0xCC,0xCC,0x00,0x00}, /* X */
    {0x00,0x00,0xCC,0xCC,0x78,0x78,0x30,0x30,0x30,0x30,0x30,0x00,0x00}, /* Y */
    {0x00,0x00,0xFC,0x0C,0x18,0x18,0x30,0x60,0x60,0xC0,0xFC,0x00,0x00}, /* Z */
    {0x00,0x78,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* [ */
    {0x00,0x00,0xC0,0xC0,0x60,0x60,0x30,0x18,0x18,0x0C,0x0C,0x00,0x00}, /* ? */
    {0x00,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* ] */
    {0x00,0x00,0x30,0x78,0xCC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFC,0x00}, /* _ */
    {0x00,0x60,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xDC,0x6C,0x00,0x00}, /* a */
    {0x00,0x00,0xC0,0xC0,0xC0,0xD8,0xEC,0xCC,0xCC,0xEC,0xD8,0x00,0x00}, /* b */
    {0x00,0x00,0x00,0x00,0x00,0x78,0xCC,0xC0,0xC0,0xCC,0x78,0x00,0x00}, /* c */
    {0x00,0x00,0x0C,0x0C,0x0C,0x6C,0xDC,0xCC,0xCC,0xDC,0x6C,0x00,0x00}, /* d */
    {0x00,0x00,0x00,0x00,0x00,0x78,0xCC,0xFC,0xC0,0xC0,0x78,0x00,0x00}, /* e */
    {0x00,0x00,0x38,0x6C,0x60,0x60,0xF8,0x60,0x60,0x60,0x60,0x00,0x00}, /* f */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xD8,0xD8,0x70,0xC0,0x78,0xCC,0x78}, /* g */
    {0x00,0x00,0xC0,0xC0,0xC0,0xD8,0xEC,0xCC,0xCC,0xCC,0xCC,0x00,0x00}, /* h */
    {0x00,0x00,0x30,0x30,0x00,0x70,0x30,0x30,0x30,0x30,0x78,0x00,0x00}, /* i */
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0xD8,0xD8,0x70}, /* j */
    {0x00,0x00,0xC0,0xC0,0xC0,0xD8,0xF0,0xE0,0xF0,0xD8,0xCC,0x00,0x00}, /* k */
    {0x00,0x00,0x70,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x78,0x00,0x00}, /* l */
    {0x00,0x00,0x00,0x00,0x00,0xF8,0xFC,0xFC,0xFC,0xFC,0xCC,0x00,0x00}, /* m */
    {0x00,0x00,0x00,0x00,0x00,0xD8,0xEC,0xCC,0xCC,0xCC,0xCC,0x00,0x00}, /* n */
    {0x00,0x00,0x00,0x00,0x00,0x78,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00}, /* o */
    {0x00,0x00,0x00,0x00,0x00,0xD8,0xEC,0xCC,0xEC,0xD8,0xC0,0xC0,0xC0}, /* p */
    {0x00,0x00,0x00,0x00,0x00,0x6C,0xDC,0xCC,0xDC,0x6C,0x0C,0x0C,0x0C}, /* q */
    {0x00,0x00,0x00,0x00,0x00,0xD8,0xEC,0xC0,0xC0,0xC0,0xC0,0x00,0x00}, /* r */
    {0x00,0x00,0x00,0x00,0x00,0x78,0xCC,0x70,0x18,0xCC,0x78,0x00,0x00}, /* s */
    {0x00,0x00,0x00,0x60,0x60,0xF8,0x60,0x60,0x60,0x6C,0x38,0x00,0x00}, /* t */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xDC,0x6C,0x00,0x00}, /* u */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0x78,0x78,0x30,0x00,0x00}, /* v */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xFC,0xFC,0xFC,0x48,0x00,0x00}, /* w */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0x78,0x30,0x30,0x78,0xCC,0x00,0x00}, /* x */
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xDC,0x6C,0x0C,0xCC,0x78}, /* y */
    {0x00,0x00,0x00,0x00,0x00,0xFC,0x18,0x30,0x60,0xC0,0xFC,0x00,0x00}, /* z */
    {0x00,0x38,0x60,0x60,0x60,0x30,0xE0,0x30,0x60,0x60,0x60,0x38,0x00}, /* { */
    {0x00,0x00,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x00,0x00}, /* | */
    {0x00,0xE0,0x30,0x30,0x30,0x60,0x38,0x60,0x30,0x30,0x30,0xE0,0x00}, /* } */
    {0x00,0x00,0x68,0xF8,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};


/* ---------------------------------------------------------------- state */
typedef struct {
    char file[160];                /* real file name (as on the card)   */
    char title[96];                /* display name: ascii, no extension */
} Track;

static Track g_tracks[MAX_TRACKS];
static int   g_ntracks = 0;

static char  g_music_dir[32] = "ms0:/MUSIC";
static char  g_app_dir[160]  = "ms0:/PSP/GAME/SoftClub/";

static volatile int g_running   = 1;   /* cleared by the HOME-menu exit callback */
static volatile int g_quit      = 0;   /* tells the audio thread to finish        */
static volatile int g_state     = ST_STOP;
static volatile int g_cur       = -1;  /* track that is loaded / playing          */
static volatile int g_elapsed   = 0;   /* seconds                                 */
static volatile int g_total     = 0;   /* seconds (estimated from bitrate)        */
static volatile int g_bitrate   = 0;   /* kbps                                    */
static volatile int g_rate      = 0;   /* Hz                                      */
static volatile int g_volume    = 80;  /* 0..100                                  */
static volatile int g_pause     = 0;
static volatile int g_req_track = -1;  /* requested track, -1 = stop              */
static volatile int g_req_seq   = 0;   /* incremented on every request            */

static int         g_theme   = 0;
static const char *g_err     = NULL;
static int         g_audio_ok = 0;

static unsigned char g_mp3buf[MP3_BUF_SIZE + 2048] __attribute__((aligned(64)));
static unsigned char g_pcmbuf[PCM_BUF_SIZE]        __attribute__((aligned(64)));
static short         g_acc[2 * 2304]               __attribute__((aligned(64)));

/* ------------------------------------------------------------- graphics */
static uint32_t  g_bg [SB * SH] __attribute__((aligned(64)));   /* pre-rendered background */
static uint32_t  g_scr[SB * SH] __attribute__((aligned(64)));   /* frame being composed    */
static uint32_t *g_dst = g_bg;                                  /* current draw target     */

static void     *g_vram_phys[2];
static uint32_t *g_vram[2];
static int       g_back = 1;

#define RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xFF000000u)

typedef struct {
    const char *name;
    uint32_t body_top, body_bot;       /* page background gradient      */
    uint32_t hero_top, hero_bot;       /* big picture area gradient     */
    uint32_t glow1, glow2, glow3;      /* soft blurry blobs             */
    uint32_t line;                     /* white-ish frame lines         */
    uint32_t head;                     /* header cells                  */
    uint32_t bar, bar_text;            /* label bars                    */
    uint32_t text;                     /* list text                     */
    uint32_t sel_bg, sel_text;         /* selected row                  */
    uint32_t accent;                   /* progress / volume fill        */
    uint32_t hero_text, hero_shadow;   /* text over the picture         */
    uint32_t well;                     /* recessed progress background  */
} Theme;

static const Theme THEMES[N_THEMES] = {
    { "SOFT CLUB",
      RGB(206,204,214), RGB(184,182,198), RGB(228,243,247), RGB(86,166,188),
      RGB(88,138,232),  RGB(255,255,255), RGB(120,212,212),
      RGB(255,255,255), RGB(166,164,182), RGB(98,106,126), RGB(255,255,255),
      RGB(44,48,64),    RGB(98,106,126),  RGB(255,255,255), RGB(112,204,218),
      RGB(255,255,255), RGB(38,92,120),   RGB(116,122,146) },
    { "GEN X",
      RGB(204,206,208), RGB(168,172,176), RGB(234,238,232), RGB(116,148,138),
      RGB(206,228,60),  RGB(255,255,255), RGB(92,150,190),
      RGB(255,255,255), RGB(148,152,156), RGB(58,64,70),    RGB(236,244,255),
      RGB(30,34,38),    RGB(58,64,70),    RGB(214,232,60),  RGB(206,226,48),
      RGB(255,255,255), RGB(46,70,64),    RGB(92,98,104) },
    { "MIDNIGHT LOUNGE",
      RGB(54,58,88),    RGB(28,30,52),    RGB(14,18,48),    RGB(60,84,156),
      RGB(196,80,200),  RGB(80,160,255),  RGB(40,220,220),
      RGB(150,160,214), RGB(66,72,110),   RGB(20,22,42),    RGB(190,206,255),
      RGB(200,210,240), RGB(92,112,200),  RGB(255,255,255), RGB(110,200,255),
      RGB(240,246,255), RGB(10,12,32),    RGB(12,14,30) },
    { "FROSTED SILVER",
      RGB(238,242,246), RGB(208,214,222), RGB(250,252,254), RGB(172,188,208),
      RGB(150,182,238), RGB(255,255,255), RGB(200,210,232),
      RGB(255,255,255), RGB(188,196,208), RGB(116,126,142), RGB(255,255,255),
      RGB(50,58,72),    RGB(116,126,142), RGB(255,255,255), RGB(84,128,205),
      RGB(58,70,98),    RGB(255,255,255), RGB(150,160,176) },
    { "PEACH FIZZ",
      RGB(247,226,216), RGB(232,200,194), RGB(255,238,222), RGB(250,158,150),
      RGB(255,198,112), RGB(255,122,172), RGB(255,255,255),
      RGB(255,255,255), RGB(224,172,172), RGB(170,102,112), RGB(255,255,255),
      RGB(84,48,56),    RGB(170,102,112), RGB(255,255,255), RGB(255,138,108),
      RGB(255,255,255), RGB(170,80,90),   RGB(190,132,132) },
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t mix(uint32_t a, uint32_t b, int t)          /* t = 0..256 */
{
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
    int r  = ar + (br - ar) * t / 256;
    int g  = ag + (bg - ag) * t / 256;
    int bl = ab + (bb - ab) * t / 256;
    return RGB(r, g, bl);
}

static void rect(int x, int y, int w, int h, uint32_t c)
{
    int x0 = clampi(x, 0, SW), x1 = clampi(x + w, 0, SW);
    int y0 = clampi(y, 0, SH), y1 = clampi(y + h, 0, SH);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *p = g_dst + yy * SB;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

static void rect_a(int x, int y, int w, int h, uint32_t c, int a)
{
    int x0 = clampi(x, 0, SW), x1 = clampi(x + w, 0, SW);
    int y0 = clampi(y, 0, SH), y1 = clampi(y + h, 0, SH);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *p = g_dst + yy * SB;
        for (int xx = x0; xx < x1; xx++) p[xx] = mix(p[xx], c, a);
    }
}

static void vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
    for (int i = 0; i < h; i++)
        rect(x, y + i, w, 1, mix(top, bot, h > 1 ? i * 256 / (h - 1) : 0));
}

static void glow(int cx, int cy, int rx, int ry, uint32_t c, int amax)
{
    for (int y = cy - ry; y <= cy + ry; y++) {
        if (y < 0 || y >= SH) continue;
        int dy = (y - cy) * 256 / ry;
        for (int x = cx - rx; x <= cx + rx; x++) {
            if (x < 0 || x >= SW) continue;
            int dx = (x - cx) * 256 / rx;
            int d2 = (dx * dx + dy * dy) >> 8;
            if (d2 >= 256) continue;
            int f  = 256 - d2;
            int a  = ((f * f) >> 8) * amax >> 8;
            if (a <= 0) continue;
            uint32_t *p = &g_dst[y * SB + x];
            *p = mix(*p, c, a);
        }
    }
}

/* rounded rectangles ---------------------------------------------------- */
static int isqrt_i(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

static int rr_ins(int h, int r, int i)       /* horizontal inset of row i */
{
    int d;
    if (r <= 0) return 0;
    if (i < r) d = i;
    else if (i >= h - r) d = h - 1 - i;
    else return 0;
    return r - isqrt_i(r * r - (r - d) * (r - d));
}

static void rrect_a(int x, int y, int w, int h, int r, uint32_t c, int a)
{
    for (int i = 0; i < h; i++) {
        int o = rr_ins(h, r, i);
        rect_a(x + o, y + i, w - 2 * o, 1, c, a);
    }
}

static void rring_a(int x, int y, int w, int h, int r, uint32_t c, int a)
{
    for (int i = 0; i < h; i++) {
        int o0 = rr_ins(h, r, i), o1 = w - o0;
        if (i == 0 || i == h - 1 || w < 3) {
            rect_a(x + o0, y + i, o1 - o0, 1, c, a);
            continue;
        }
        int ri = r > 0 ? r - 1 : 0;
        int i0 = 1 + rr_ins(h - 2, ri, i - 1);
        int i1 = (w - 1) - rr_ins(h - 2, ri, i - 1);
        if (i0 < o0) i0 = o0;
        if (i1 > o1) i1 = o1;
        rect_a(x + o0, y + i, i0 - o0, 1, c, a);
        rect_a(x + i1, y + i, o1 - i1, 1, c, a);
    }
}

/* text ------------------------------------------------------------------ */
typedef struct { const unsigned char *data; int w, h; } Font;
static const Font F_S = { &FONT_S[0][0], 6, 10 };
static const Font F_B = { &FONT_B[0][0], 6, 13 };

static int text_w(const Font *f, const char *s, int scale) { return (int)strlen(s) * f->w * scale; }

static void text_clip(const Font *f, int x, int y, const char *s, uint32_t c,
                      int scale, int cx0, int cx1)
{
    if (cx0 < 0) cx0 = 0;
    if (cx1 > SW) cx1 = SW;
    for (; *s; s++, x += f->w * scale) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        if (x + f->w * scale <= cx0 || x >= cx1) continue;
        const unsigned char *g = f->data + (ch - 32) * f->h;
        for (int ry = 0; ry < f->h; ry++) {
            unsigned bits = g[ry];
            if (!bits) continue;
            for (int rx = 0; rx < f->w; rx++) {
                if (!(bits & (0x80u >> rx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int yy = y + ry * scale + sy;
                    if ((unsigned)yy >= SH) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int xx = x + rx * scale + sx;
                        if (xx >= cx0 && xx < cx1) g_dst[yy * SB + xx] = c;
                    }
                }
            }
        }
    }
}

static void txt(const Font *f, int x, int y, const char *s, uint32_t c)
{
    text_clip(f, x, y, s, c, 1, 0, SW);
}

static void txt_bold(const Font *f, int x, int y, const char *s, uint32_t c)
{
    text_clip(f, x, y, s, c, 1, 0, SW);
    text_clip(f, x + 1, y, s, c, 1, 0, SW);
}

static void txt_right(const Font *f, int right, int y, const char *s, uint32_t c)
{
    txt_bold(f, right - text_w(f, s, 1) - 1, y, s, c);
}

static void txt_shadow(const Font *f, int x, int y, const char *s, uint32_t c, uint32_t sh,
                       int scale, int cx0, int cx1)
{
    int off = scale >= 2 ? 2 : 1;
    text_clip(f, x + off, y + off, s, sh, scale, cx0, cx1);
    text_clip(f, x, y, s, c, scale, cx0, cx1);
}

/* -------------------------------------------------------------- layout */
#define HDR_Y    8
#define HDR_H    18
#define HERO_X   8
#define HERO_Y   29
#define HERO_W   463
#define HERO_H   108
#define LOW_Y    141
#define LOW_H    125
#define LIST_X   8
#define LIST_W   222
#define LROW_Y   155
#define LROW_H   12
#define RP_X     234
#define RP_W     237
#define PROG_X   242
#define PROG_Y   160
#define PROG_W   220
#define PROG_H   10
#define VOL_Y    194
#define VOL_CELLS 20

static void render_bg(const Theme *t)
{
    char buf[64];
    g_dst = g_bg;

    vgrad(0, 0, SW, SH, t->body_top, t->body_bot);

    /* white double frame around everything */
    rect_a(3, 3, 474, 2, t->line, 235);
    rect_a(3, 267, 474, 2, t->line, 235);
    rect_a(3, 5, 2, 262, t->line, 235);
    rect_a(475, 5, 2, 262, t->line, 235);

    /* header cells */
    rrect_a(8, HDR_Y, 268, HDR_H, 3, t->head, 255);
    rring_a(8, HDR_Y, 268, HDR_H, 3, t->line, 230);
    rrect_a(280, HDR_Y, 191, HDR_H, 3, t->head, 255);
    rring_a(280, HDR_Y, 191, HDR_H, 3, t->line, 230);
    txt_bold(&F_B, 16, HDR_Y + 3, "SOFT//CLUB", t->bar_text);
    txt(&F_S, 16 + 11 * 6 + 8, HDR_Y + 5, "mp3 player // psp", t->bar_text);
    snprintf(buf, sizeof buf, "%s  %d FILES", g_music_dir, g_ntracks);
    txt_right(&F_S, 467, HDR_Y + 5, buf, t->bar_text);

    /* hero picture */
    vgrad(HERO_X, HERO_Y, HERO_W, HERO_H, t->hero_top, t->hero_bot);
    glow(130,  62, 120, 78, t->glow1, 190);
    glow(340,  98, 190, 66, t->glow2, 120);
    glow(430,  44,  96, 58, t->glow3, 130);
    glow( 60, 122, 110, 40, t->glow3,  90);
    glow(250,  40,  70, 40, t->glow2,  80);
    for (int k = 0; k < 7; k++)                       /* faint horizon / water lines */
        rect_a(HERO_X, 98 + k * 5 + (k * k) / 3, HERO_W, 1, t->glow2, 120 - k * 14);
    rring_a(HERO_X, HERO_Y, HERO_W, HERO_H, 2, t->line, 235);

    /* playlist panel */
    rrect_a(LIST_X, LOW_Y, LIST_W, LOW_H, 4, t->line, 70);
    rring_a(LIST_X, LOW_Y, LIST_W, LOW_H, 4, t->line, 230);
    rect(LIST_X + 1, LOW_Y + 1, LIST_W - 2, 13, t->bar);
    txt_bold(&F_S, LIST_X + 6, LOW_Y + 3, "PLAYLIST", t->bar_text);
    for (int r = 0; r < LIST_ROWS; r++)
        rect_a(LIST_X + 3, LROW_Y + r * LROW_H + LROW_H - 1, LIST_W - 15, 1, t->line, 110);

    /* right panel */
    rrect_a(RP_X, LOW_Y, RP_W, LOW_H, 4, t->line, 70);
    rring_a(RP_X, LOW_Y, RP_W, LOW_H, 4, t->line, 230);

    rect(RP_X + 1, LOW_Y + 1, RP_W - 2, 13, t->bar);
    txt_bold(&F_S, RP_X + 6, LOW_Y + 3, "TIME", t->bar_text);
    rrect_a(PROG_X - 2, PROG_Y - 2, PROG_W + 4, PROG_H + 4, 3, t->well, 255);
    rring_a(PROG_X - 2, PROG_Y - 2, PROG_W + 4, PROG_H + 4, 3, t->line, 170);

    rect(RP_X + 1, 177, RP_W - 2, 13, t->bar);
    txt_bold(&F_S, RP_X + 6, 179, "VOLUME", t->bar_text);
    for (int i = 0; i < VOL_CELLS; i++)
        rrect_a(241 + i * 11, VOL_Y, 9, 10, 2, t->well, 255);

    rect(RP_X + 1, 211, RP_W - 2, 13, t->bar);
    txt_bold(&F_S, RP_X + 6, 213, "THEME", t->bar_text);

    txt(&F_S, RP_X + 8, 229, "X PLAY/PAUSE    [] STOP",     t->text);
    txt(&F_S, RP_X + 8, 240, "L/R PREV/NEXT   /\\ THEME",   t->text);
    txt(&F_S, RP_X + 8, 251, "UP/DN LIST      </> VOLUME",  t->text);
}

/* ------------------------------------------------------------ dynamic UI */
typedef struct {
    int state, cur, elapsed, total, bitrate, rate;
    int vol, theme, sel, top, tick, ntr;
} UiState;

static void fmt_time(char *out, size_t cap, int s, int known)
{
    if (!known) { snprintf(out, cap, "--:--"); return; }
    snprintf(out, cap, "%02d:%02d", s / 60, s % 60);
}

static const char *title_for(const UiState *u)
{
    if (g_err)           return g_err;
    if (u->ntr == 0)     return "NO MP3 FILES FOUND";
    if (u->cur >= 0)     return g_tracks[u->cur].title;
    return "PRESS X TO PLAY";
}

static void render_ui(const UiState *u)
{
    const Theme *t = &THEMES[u->theme];
    char buf[96], a[16], b[16];

    memcpy(g_scr, g_bg, sizeof g_scr);
    g_dst = g_scr;

    /* ---- hero: title (scrolls if long) */
    const char *title = title_for(u);
    int tw = text_w(&F_B, title, 2);
    if (tw <= 447) {
        txt_shadow(&F_B, 16, 36, title, t->hero_text, t->hero_shadow, 2, 16, 463);
    } else {
        int period = tw + 72;
        int off = (u->tick * 2) % period;
        txt_shadow(&F_B, 16 - off,          36, title, t->hero_text, t->hero_shadow, 2, 16, 463);
        txt_shadow(&F_B, 16 - off + period, 36, title, t->hero_text, t->hero_shadow, 2, 16, 463);
    }

    /* ---- hero: info line */
    if (u->cur >= 0 && u->ntr > 0 && !g_err) {
        if (u->bitrate > 0)
            snprintf(buf, sizeof buf, "TRACK %02d/%02d   %d KBPS   %d.%d KHZ",
                     u->cur + 1, u->ntr, u->bitrate, u->rate / 1000, (u->rate % 1000) / 100);
        else
            snprintf(buf, sizeof buf, "TRACK %02d/%02d", u->cur + 1, u->ntr);
        txt_shadow(&F_S, 17, 66, buf, t->hero_text, t->hero_shadow, 1, 0, SW);
    }

    /* ---- hero: big time + state word */
    fmt_time(a, sizeof a, u->elapsed, u->cur >= 0);
    fmt_time(b, sizeof b, u->total, u->total > 0);
    snprintf(buf, sizeof buf, "%s / %s", a, b);
    txt_shadow(&F_B, 16, 102, buf, t->hero_text, t->hero_shadow, 2, 0, SW);

    const char *word = "//ready";
    switch (u->state) {
        case ST_LOAD:  word = "//loading"; break;
        case ST_PLAY:  word = "//playing"; break;
        case ST_PAUSE: word = "//paused";  break;
        case ST_ERROR: word = "//error";   break;
        default:       word = (u->cur >= 0) ? "//stopped" : "//ready"; break;
    }
    txt_shadow(&F_B, 463 - text_w(&F_B, word, 3), 94, word, t->hero_text, t->hero_shadow, 3, 0, SW);

    /* ---- playlist */
    if (u->ntr == 0) {
        txt(&F_S, 16, LROW_Y + 4,  "NO MP3 FILES FOUND.", t->text);
        txt(&F_S, 16, LROW_Y + 18, "COPY YOUR MUSIC TO:", t->text);
        txt_bold(&F_S, 16, LROW_Y + 32, g_music_dir, t->text);
    }
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = u->top + r;
        if (idx >= u->ntr) break;
        int ry = LROW_Y + r * LROW_H;
        int is_sel = (idx == u->sel);
        uint32_t col = t->text;
        if (is_sel) {
            rect(LIST_X + 3, ry, LIST_W - 15, LROW_H - 1, t->sel_bg);
            col = t->sel_text;
        }
        char mark = ' ';
        if (idx == u->cur) mark = (u->state == ST_PAUSE) ? '=' : (u->state == ST_STOP ? ' ' : '>');
        char name[40];
        const char *src = g_tracks[idx].title;
        if (strlen(src) > 30) { snprintf(name, sizeof name, "%.28s..", src); }
        else                  { snprintf(name, sizeof name, "%.30s", src); }
        snprintf(buf, sizeof buf, "%c%02d %s", mark, idx + 1, name);
        txt(&F_S, LIST_X + 7, ry + 1, buf, col);
        if (mark != ' ') {
            char m[2] = { mark, 0 };
            txt(&F_S, LIST_X + 7, ry + 1, m, is_sel ? col : t->accent);
        }
    }
    if (u->ntr > LIST_ROWS) {                       /* scrollbar */
        int th = clampi(106 * LIST_ROWS / u->ntr, 10, 106);
        int ty = 155 + (106 - th) * u->top / (u->ntr - LIST_ROWS);
        rect_a(LIST_X + LIST_W - 9, 155, 4, 106, t->line, 100);
        rect(LIST_X + LIST_W - 9, ty, 4, th, t->bar);
    }

    /* ---- right panel: time / progress */
    int pct = (u->total > 0) ? clampi(u->elapsed * 100 / u->total, 0, 100) : 0;
    if (u->total > 0) snprintf(buf, sizeof buf, "%d%%", pct); else snprintf(buf, sizeof buf, "--%%");
    txt_right(&F_S, RP_X + RP_W - 6, LOW_Y + 3, buf, t->bar_text);
    if (u->total > 0) {
        int fw = PROG_W * pct / 100;
        rect(PROG_X, PROG_Y, fw, PROG_H, t->accent);
        rect_a(PROG_X, PROG_Y, fw, 4, t->line, 90);
    }

    /* ---- volume */
    snprintf(buf, sizeof buf, "%d%%", u->vol);
    txt_right(&F_S, RP_X + RP_W - 6, 179, buf, t->bar_text);
    int cells = (u->vol * VOL_CELLS + 50) / 100;
    for (int i = 0; i < cells; i++) {
        rrect_a(241 + i * 11, VOL_Y, 9, 10, 2, t->accent, 255);
        rect_a(241 + i * 11 + 1, VOL_Y + 1, 7, 3, t->line, 90);
    }

    /* ---- theme */
    snprintf(buf, sizeof buf, "%s  %d/%d", t->name, u->theme + 1, N_THEMES);
    txt_right(&F_S, RP_X + RP_W - 6, 213, buf, t->bar_text);
}

static void init_display(void)
{
    g_vram_phys[0] = sceGeEdramGetAddr();
    g_vram_phys[1] = (void *)((uintptr_t)g_vram_phys[0] + FRAME_BYTES);
    g_vram[0] = (uint32_t *)((uintptr_t)g_vram_phys[0] | 0x40000000);
    g_vram[1] = (uint32_t *)((uintptr_t)g_vram_phys[1] | 0x40000000);
    memset(g_vram[0], 0, FRAME_BYTES);
    memset(g_vram[1], 0, FRAME_BYTES);
    sceDisplaySetMode(0, SW, SH);
    sceDisplaySetFrameBuf(g_vram_phys[0], SB, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
}

static void present(void)
{
    memcpy(g_vram[g_back], g_scr, FRAME_BYTES);
    sceDisplaySetFrameBuf(g_vram_phys[g_back], SB, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    g_back ^= 1;
}

/* --------------------------------------------------- file names -> ascii */
static const char *const CYR[32] = {
    "a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
    "r","s","t","u","f","kh","ts","ch","sh","shch","","y","","e","yu","ya"
};

static void build_title(const char *src, char *dst, size_t cap)
{
    const unsigned char *s = (const unsigned char *)src;
    size_t n = strlen(src), o = 0, i = 0;
    if (n > 4) n -= 4;                                   /* drop ".mp3" */

    while (i < n && o + 6 < cap) {
        unsigned c = s[i], cp, len;
        if (c < 0x80)                          { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = 0xFFFF; len = 4; }
        else                                   { cp = 0xFFFF; len = 1; }
        i += len;

        char tmp[8];
        const char *rep = "?";
        if (cp < 0x80) {
            tmp[0] = (cp >= 32 && cp < 127) ? (char)cp : '_';
            tmp[1] = 0;
            rep = tmp;
        } else if (cp >= 0x410 && cp <= 0x42F) {         /* А-Я */
            snprintf(tmp, sizeof tmp, "%s", CYR[cp - 0x410]);
            if (tmp[0] >= 'a' && tmp[0] <= 'z') tmp[0] = (char)(tmp[0] - 32);
            rep = tmp;
        } else if (cp >= 0x430 && cp <= 0x44F) {         /* а-я */
            rep = CYR[cp - 0x430];
        } else switch (cp) {
            case 0x401: rep = "Yo"; break;   case 0x451: rep = "yo"; break;
            case 0x406: rep = "I";  break;   case 0x456: rep = "i";  break;
            case 0x407: rep = "Yi"; break;   case 0x457: rep = "yi"; break;
            case 0x404: rep = "Ye"; break;   case 0x454: rep = "ye"; break;
            case 0x490: rep = "G";  break;   case 0x491: rep = "g";  break;
            case 0x2116: rep = "No"; break;
            case 0xA0:   rep = " "; break;
            case 0x2018: case 0x2019: rep = "'"; break;
            case 0x201C: case 0x201D: rep = "\""; break;
            case 0x2013: case 0x2014: rep = "-"; break;
            case 0x2026: rep = "..."; break;
            default: rep = "?"; break;
        }
        while (*rep && o + 1 < cap) dst[o++] = *rep++;
    }
    dst[o] = 0;
}

static int cmp_tracks(const void *a, const void *b)
{
    return strcasecmp(((const Track *)a)->file, ((const Track *)b)->file);
}

static void scan_music(void)
{
    SceUID d = sceIoDopen(g_music_dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (g_ntracks < MAX_TRACKS && sceIoDread(d, &e) > 0) {
        size_t l = strlen(e.d_name);
        if (!FIO_S_ISDIR(e.d_stat.st_mode) && l > 4 && l < sizeof g_tracks[0].file &&
            strcasecmp(e.d_name + l - 4, ".mp3") == 0) {
            Track *t = &g_tracks[g_ntracks++];
            snprintf(t->file, sizeof t->file, "%.159s", e.d_name);
            build_title(e.d_name, t->title, sizeof t->title);
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    qsort(g_tracks, g_ntracks, sizeof g_tracks[0], cmp_tracks);
}

/* -------------------------------------------------------------- settings */
static void cfg_path(char *out, size_t cap) { snprintf(out, cap, "%ssettings.cfg", g_app_dir); }

static void cfg_load(void)
{
    char path[200], buf[32];
    cfg_path(path, sizeof path);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    memset(buf, 0, sizeof buf);
    sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    int th = 0, vol = 80;
    if (sscanf(buf, "%d %d", &th, &vol) == 2) {
        g_theme  = clampi(th, 0, N_THEMES - 1);
        g_volume = clampi(vol, 0, 100);
    }
}

static void cfg_save(void)
{
    char path[200], buf[32];
    cfg_path(path, sizeof path);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    int n = snprintf(buf, sizeof buf, "%d %d\n", g_theme, (int)g_volume);
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

static void init_paths(const char *argv0)
{
    if (!argv0) return;
    const char *colon = strchr(argv0, ':');
    if (colon && (colon - argv0) < 8) {
        snprintf(g_music_dir, sizeof g_music_dir, "%.*s/MUSIC", (int)(colon - argv0 + 1), argv0);
    }
    const char *slash = strrchr(argv0, '/');
    if (slash && (size_t)(slash - argv0 + 1) < sizeof g_app_dir) {
        snprintf(g_app_dir, sizeof g_app_dir, "%.*s", (int)(slash - argv0 + 1), argv0);
    }
}

/* ======================================================================
 *  AUDIO THREAD
 * ====================================================================== */
static int vol_hw(int v) { return v * v * 0x8000 / 10000; }

/* read the next chunk of the file into the decoder's stream buffer */
static int fill_stream(SceUID fd, SceInt32 h)
{
    SceUChar8 *dst = NULL;
    SceInt32 towrite = 0, srcpos = 0;

    if (sceMp3GetInfoToAddStreamData(h, &dst, &towrite, &srcpos) < 0) return -1;
    if (towrite <= 0) return 0;
    if (sceIoLseek32(fd, (int)srcpos, PSP_SEEK_SET) < 0) return -1;
    int n = sceIoRead(fd, dst, (SceSize)towrite);
    if (n < 0) return -1;
    if (n == 0) return 0;                                 /* end of file */
    if (sceMp3NotifyAddStreamData(h, n) < 0) return -1;
    return n;
}

/* find where the MP3 audio really starts / ends (skip ID3v2 / ID3v1) */
static void find_stream(SceUID fd, int size, int *start, int *end)
{
    unsigned char hdr[10], tag[3];
    *start = 0;
    *end = size;

    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    if (sceIoRead(fd, hdr, 10) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        int sz = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        *start = 10 + sz + ((hdr[5] & 0x10) ? 10 : 0);
    }
    if (size > 128) {
        sceIoLseek32(fd, size - 128, PSP_SEEK_SET);
        if (sceIoRead(fd, tag, 3) == 3 && memcmp(tag, "TAG", 3) == 0) *end = size - 128;
    }
    if (*start >= *end) { *start = 0; return; }

    /* look for the first real frame header, tolerate padding after the tag */
    static unsigned char probe[4096];
    sceIoLseek32(fd, *start, PSP_SEEK_SET);
    int n = sceIoRead(fd, probe, sizeof probe);
    for (int i = 0; i + 4 <= n; i++) {
        if (probe[i] == 0xFF && (probe[i + 1] & 0xE0) == 0xE0 &&
            (probe[i + 1] & 0x18) != 0x08 && (probe[i + 1] & 0x06) != 0x00 &&
            (probe[i + 2] & 0xF0) != 0xF0 && (probe[i + 2] & 0x0C) != 0x0C) {
            *start += i;
            break;
        }
    }
}

static int play_track(int idx, int *seen)
{
    int result = PR_ERROR;
    SceUID fd = -1;
    SceInt32 h = -1;
    int src_open = 0, decoded_any = 0, acc_n = 0;
    char path[320];

    snprintf(path, sizeof path, "%s/%s", g_music_dir, g_tracks[idx].file);

    g_cur = idx;
    g_elapsed = 0; g_total = 0; g_bitrate = 0; g_rate = 0;
    g_pause = 0;
    g_state = ST_LOAD;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) goto done;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    if (size <= 0) goto done;

    int start, end;
    find_stream(fd, size, &start, &end);

    SceMp3InitArg arg;
    memset(&arg, 0, sizeof arg);
    arg.mp3StreamStart = start;
    arg.mp3StreamEnd   = end;
    arg.mp3Buf         = g_mp3buf;
    arg.mp3BufSize     = MP3_BUF_SIZE;
    arg.pcmBuf         = g_pcmbuf;
    arg.pcmBufSize     = PCM_BUF_SIZE;

    h = sceMp3ReserveMp3Handle(&arg);
    if (h < 0) goto done;
    if (fill_stream(fd, h) < 0) goto done;
    if (sceMp3Init(h) < 0) goto done;
    sceMp3SetLoopNum(h, 0);

    int rate = sceMp3GetSamplingRate(h);
    int ch   = sceMp3GetMp3ChannelNum(h);
    int br   = sceMp3GetBitRate(h);
    if (rate <= 0 || ch < 1 || ch > 2) goto done;

    g_rate = rate;
    g_bitrate = br;
    g_total = (br > 0) ? (end - start) / (br * 125) : 0;

    if (sceAudioSRCChReserve(OUT_BLOCK, rate, 2) < 0) goto done;
    src_open = 1;

    g_state = ST_PLAY;

    for (;;) {
        if (g_quit)                 { result = PR_QUIT;    break; }
        if (g_req_seq != *seen)     { result = PR_REQUEST; break; }
        if (g_pause) {
            g_state = ST_PAUSE;
            sceKernelDelayThread(20000);
            continue;
        }
        g_state = ST_PLAY;

        if (sceMp3CheckStreamDataNeeded(h) > 0) fill_stream(fd, h);

        SceShort16 *pcm = NULL;
        int bytes = sceMp3Decode(h, &pcm);
        if (bytes <= 0 || pcm == NULL) {
            result = decoded_any ? PR_ENDED : PR_ERROR;
            break;
        }
        decoded_any = 1;
        sceKernelDcacheWritebackInvalidateRange(pcm, bytes);

        int samples = bytes / (2 * ch);
        if (acc_n + samples > 2304) samples = 2304 - acc_n;
        if (ch == 2) {
            memcpy(g_acc + 2 * acc_n, pcm, (size_t)samples * 4);
        } else {
            for (int i = 0; i < samples; i++) {
                g_acc[2 * (acc_n + i)]     = pcm[i];
                g_acc[2 * (acc_n + i) + 1] = pcm[i];
            }
        }
        acc_n += samples;

        while (acc_n >= OUT_BLOCK) {
            sceAudioSRCOutputBlocking(vol_hw(g_volume), g_acc);
            acc_n -= OUT_BLOCK;
            memmove(g_acc, g_acc + 2 * OUT_BLOCK, (size_t)acc_n * 4);
        }
        g_elapsed = sceMp3GetSumDecodedSample(h) / rate;
    }

    if (result == PR_ENDED && acc_n > 0) {                 /* play the last bit */
        memset(g_acc + 2 * acc_n, 0, (size_t)(OUT_BLOCK - acc_n) * 4);
        sceAudioSRCOutputBlocking(vol_hw(g_volume), g_acc);
    }

done:
    if (src_open)  sceAudioSRCChRelease();
    if (h >= 0)    sceMp3ReleaseMp3Handle(h);
    if (fd >= 0)   sceIoClose(fd);
    if (result == PR_ERROR) g_state = ST_ERROR;
    return result;
}

static int audio_thread(SceSize args, void *argp)
{
    int seen = g_req_seq;
    (void)args; (void)argp;

    while (!g_quit) {
        if (g_req_seq == seen) { sceKernelDelayThread(20000); continue; }

        seen = g_req_seq;
        int track = g_req_track;

        while (track >= 0 && track < g_ntracks && !g_quit) {
            int res = play_track(track, &seen);
            if (res == PR_ENDED) {
                track = (track + 1) % g_ntracks;               /* repeat-all */
            } else if (res == PR_REQUEST) {
                seen = g_req_seq;
                track = g_req_track;
            } else {
                track = -2;                                    /* error / quit */
            }
        }
        if (track == -1 && !g_quit) {                          /* explicit stop */
            g_state = ST_STOP;
            g_elapsed = 0;
        }
    }
    return 0;
}

static void req_play(int idx)
{
    if (!g_audio_ok) return;
    g_req_track = idx;
    g_req_seq = g_req_seq + 1;
}

/* ------------------------------------------------------------- callbacks */
static int exit_cb(int a1, int a2, void *common)
{
    (void)a1; (void)a2; (void)common;
    g_running = 0;
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int id = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int t = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ---------------------------------------------------------------- input */
static int rep(unsigned btn, unsigned bit, int *hold)   /* press + auto-repeat */
{
    if (!(btn & bit)) { *hold = 0; return 0; }
    (*hold)++;
    if (*hold == 1) return 1;
    return (*hold > 20 && ((*hold - 20) % 4) == 0);
}

int main(int argc, char **argv)
{
    SceUID audio_th = -1;

    setup_callbacks();
    scePowerSetClockFrequency(266, 266, 133);
    init_paths(argc > 0 ? argv[0] : NULL);
    cfg_load();
    scan_music();

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    init_display();

    /* MP3 decoder modules (need custom firmware / PSP-1000+ firmware features) */
    if (sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC) >= 0 &&
        sceUtilityLoadModule(PSP_MODULE_AV_MP3) >= 0 &&
        sceMp3InitResource() >= 0) {
        audio_th = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
        if (audio_th >= 0 && sceKernelStartThread(audio_th, 0, NULL) >= 0) g_audio_ok = 1;
    }
    if (!g_audio_ok) g_err = "MP3 DECODER NOT AVAILABLE";

    render_bg(&THEMES[g_theme]);

    UiState last;
    memset(&last, 0xFF, sizeof last);
    int sel = 0, top = 0, prev_cur = -1, frame = 0;
    unsigned old = 0;
    int hold[4] = { 0, 0, 0, 0 };

    while (g_running) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned btn = pad.Buttons, down = btn & ~old;
        old = btn;
        int n = g_ntracks;
        int cur = g_cur;

        if (rep(btn, PSP_CTRL_UP,    &hold[0]) && sel > 0)     sel--;
        if (rep(btn, PSP_CTRL_DOWN,  &hold[1]) && sel < n - 1) sel++;
        if (rep(btn, PSP_CTRL_LEFT,  &hold[2])) g_volume = clampi(g_volume - 5, 0, 100);
        if (rep(btn, PSP_CTRL_RIGHT, &hold[3])) g_volume = clampi(g_volume + 5, 0, 100);

        if ((down & PSP_CTRL_CROSS) && n > 0) {
            int st = g_state;
            if (sel == cur && (st == ST_PLAY || st == ST_PAUSE)) g_pause = !g_pause;
            else req_play(sel);
        }
        if ((down & PSP_CTRL_CIRCLE) && (g_state == ST_PLAY || g_state == ST_PAUSE)) g_pause = !g_pause;
        if ((down & PSP_CTRL_SQUARE) && n > 0) req_play(-1);
        if ((down & PSP_CTRL_LTRIGGER) && n > 0) {
            int base = (cur >= 0) ? cur : sel;
            sel = (base + n - 1) % n;
            req_play(sel);
        }
        if ((down & PSP_CTRL_RTRIGGER) && n > 0) {
            int base = (cur >= 0) ? cur : sel;
            sel = (base + 1) % n;
            req_play(sel);
        }
        if (down & PSP_CTRL_TRIANGLE) {
            g_theme = (g_theme + 1) % N_THEMES;
            render_bg(&THEMES[g_theme]);
            cfg_save();
        }

        /* selection follows the playing track when the player advances by itself */
        cur = g_cur;
        if (cur != prev_cur) {
            if (prev_cur < 0 || sel == prev_cur) sel = cur >= 0 ? cur : sel;
            prev_cur = cur;
        }
        sel = clampi(sel, 0, n > 0 ? n - 1 : 0);
        if (sel < top) top = sel;
        if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
        if (top < 0) top = 0;

        UiState u;
        u.state = g_state;  u.cur = cur;
        u.elapsed = g_elapsed; u.total = g_total;
        u.bitrate = g_bitrate; u.rate = g_rate;
        u.vol = g_volume;   u.theme = g_theme;
        u.sel = sel;        u.top = top;
        u.ntr = n;
        u.tick = 0;
        if (u.cur >= 0 && n > 0 && text_w(&F_B, g_tracks[u.cur].title, 2) > 447) u.tick = frame / 3;

        if (memcmp(&u, &last, sizeof u) != 0) {
            render_ui(&u);
            present();
            last = u;
        }
        frame++;
        sceDisplayWaitVblankStart();
    }

    /* shut down cleanly */
    g_quit = 1;
    if (audio_th >= 0) {
        sceKernelWaitThreadEnd(audio_th, NULL);
        sceKernelDeleteThread(audio_th);
    }
    if (g_audio_ok) sceMp3TermResource();
    cfg_save();
    sceKernelExitGame();
    return 0;
}
