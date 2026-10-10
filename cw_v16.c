#pragma GCC optimize ("Os", "no-tree-loop-distribute-patterns")
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_api.h"

#define TICK_MS     50
#define RX_TICK     8
#define MSG_MAX     18
#define HIST_N      131
#define BOX_LINES   4       /* visible message-box lines (rows 1..4) */
#define BOX_TOP     1
#define ROW_IN      5       /* compose row: just above the status row */
#define BOX_CH      16
#define TAP_MS      900
#define UNIT_DEF    100
#define UNIT_MIN    24
#define UNIT_MAX    300
#define UNIT_STEP   5
#define AF_BEEP     3
#define TONE_HZ     700
#define HOLD_MS     1500
#define POP_TICKS   (1500 / TICK_MS)   /* on-screen prompt lifetime (~1.5 s) */
#define EXIT_TICKS  (1500 / TICK_MS)   /* EXIT arm window in main-loop ticks   */
#define CFG_MAGIC   0xD7
#define SQL_DEF     5

#define F_RUN   (1<<0)
#define F_TONE  (1<<1)
#define F_FDOWN (1<<4)
#define F_MARK  (1<<5)
#define F_PEND  (1<<6)
#define F_SQL   (1<<7)

#define GPIO_REG   0x33u
#define APP_KEY_SIDE2 17u
#define APP_KEY_SIDE1 18u

void *memset(void *s, int c, size_t n){
    uint8_t *p = (uint8_t*)s;
    while(n--) *p++ = (uint8_t)c;
    return s;
}

static const uint8_t M_LET[26]={0x05,0x18,0x1A,0x0C,0x02,0x12,0x0E,0x10,0x04,0x17,0x0D,0x14,0x07,0x06,0x0F,0x16,0x1D,0x0A,0x08,0x03,0x09,0x11,0x0B,0x19,0x1B,0x1C};
static const uint8_t M_DIG[10]={0x3F,0x2F,0x27,0x23,0x21,0x20,0x30,0x38,0x3C,0x3E};
/* Key index -> multitap set. A flat blob cannot work here: adjacent literals
 * are concatenated with only one final NUL, so getFlatStr-style scanning
 * needs explicit separators (like POP_FLAT has). A pointer table is clearer. */
static const char *const KEYMAP[10] = {"", "1.0/", "ABC2", "DEF3", "GHI4",
    "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"};
static const char PROSIGN_T[10][6]={"","CQ","DE","UR","RST","R","TU","73","AR","SK"};
static const char POP_FLAT[]="BUSY\0""FULL\0""TONE\0""CARR\0""EXIT?";
static const app_api_t *A;

static struct {
    uint16_t tapMs, fTapMs, unitMs, fHoldMs, pttHoldMs, rxRun, sinceChar;
    int16_t  rxFloor, rxPeak, smooth_rssi, sqlFloor;
    uint8_t  flags, lastKey, tapIdx, prevKey, rxCode, rxAdapt, batTimer;
    uint8_t  sqlLevel, blank, firstMark, scroll, msgLen, hn, rxNew, pop, popT;
    uint8_t  menu, edit, exArm;
    uint8_t  txPos, rxCount, blink;
    char     msg[MSG_MAX+1];
    char     txPat[9];
    char     hist[HIST_N][MSG_MAX+1];
    uint8_t  hrx[HIST_N], hlen[HIST_N];
} G;

static uint32_t udiv(uint32_t n, uint32_t d, uint32_t *rem){
    uint32_t q = 0, r = 0;
    for(int8_t i = 31; i >= 0; i--){
        r = (r << 1) | ((n >> i) & 1u);
        if(r >= d){ r -= d; q |= (1u << i); }
    }
    if(rem) *rem = r;
    return q;
}

static uint8_t slen(const char *s){ uint8_t n = 0; while(s[n]) n++; return n; }

static const char *getFlatStr(const char *buf, uint8_t idx){
    while(idx--) while(*buf++) {}
    return buf;
}

static char *pstr(char *d, const char *s){
    while((*d = *s++)) d++;
    return d;
}

static char *pnum(char *d, uint32_t v, uint8_t pad){
    char t[10]; int n = 0;
    do { uint32_t r; v = udiv(v, 10, &r); t[n++] = (char)('0' + r); } while(v);
    while(n < pad) t[n++] = '0';
    while(n--) *d++ = t[n];
    *d = 0;
    return d;
}

static uint8_t morseByte(char c){
    if(c >= 'a' && c <= 'z') c -= 32;
    if(c >= 'A' && c <= 'Z') return M_LET[c - 'A'];
    if(c >= '0' && c <= '9') return M_DIG[c - '0'];
    if(c == '/') return 0x32;
    if(c == '.') return 0x55;
    return 0;
}

static char morseChar(uint8_t code){
    if(code < 2) return 0;
    for(uint8_t i=0; i<26; i++) if(M_LET[i] == code) return (char)('A' + i);
    for(uint8_t i=0; i<10; i++) if(M_DIG[i] == code) return (char)('0' + i);
    if(code == 0x32) return '/';
    if(code == 0x55) return '.';
    return '?';
}

static void beep(void){
    A->prepare_tone();
    A->bk_write(0x70u, 0xE000u);
    A->play_tone_raw(1000, 150);
    A->tones_off_rx();
    A->set_af((uint8_t)((G.flags >> 7) & 1u));
}
static void beepDeny(void){
    A->prepare_tone();
    A->bk_write(0x70u, 0xE000u);
    A->play_tone_raw(300, 110);
    A->play_tone_raw(300, 110);
    A->tones_off_rx();
    A->set_af((uint8_t)((G.flags >> 7) & 1u));
}
static void alert(uint8_t idx){
    G.pop = idx; G.popT = POP_TICKS;
    /* never beep while receiving: the tone disrupts the RX chain and the
     * incoming message is lost. BUSY is shown as a popup only. */
    if(!(G.flags & F_SQL)){ if(idx <= 1) beepDeny(); else beep(); }
}

static void blitAll(void){ A->blit_status(); A->blit_full(); }

static void chrome(void){
    A->display_clear(); A->status_clear();
    A->print_inverse((G.flags & F_TONE) ? "TONE" : "CW", 2, 0, true, true, 30);
    A->draw_battery();
}

/* ---- message history: received bold, sent inverted + "TX " ---- */
static void histNew(bool rx){
    if(G.hn >= HIST_N){
        for(uint8_t i = 0; i < HIST_N - 1; i++){
            for(uint8_t j = 0; j <= MSG_MAX; j++) G.hist[i][j] = G.hist[i+1][j];
            G.hrx[i] = G.hrx[i+1]; G.hlen[i] = G.hlen[i+1];
        }
        G.hn = HIST_N - 1;
    }
    G.hist[G.hn][0] = 0; G.hrx[G.hn] = rx ? 1 : 0; G.hlen[G.hn] = 0;
    G.hn++;
}
static void histApp(char c){
    if(!c || G.hn == 0) return;
    uint8_t k = G.hn - 1;
    if(G.hlen[k] < MSG_MAX){ G.hist[k][G.hlen[k]++] = c; G.hist[k][G.hlen[k]] = 0; }
}
static void histPrune(void){
    if(G.hn == 0) return;
    uint8_t k = G.hn - 1;
    if(!G.hrx[k]) return;                       /* only drop received bursts */
    for(uint8_t i = 0; i < G.hlen[k]; i++){ char c = G.hist[k][i]; if(c != 'T' && c != 'E') return; }
    G.hn--;
}
static uint8_t msgLines(uint8_t i){
    uint8_t n = (G.hrx[i] ? 0 : 3) + G.hlen[i];   /* "TX " prefix */
    return (uint8_t)((n + BOX_CH - 1) / BOX_CH);
}
static uint8_t boxTotalLines(void){
    uint8_t t = 0;
    for(uint8_t i = 0; i < G.hn; i++) t += msgLines(i);
    return t;
}
static void drawBoxLine(uint8_t i, uint8_t k, uint8_t row){
    char full[3 + MSG_MAX + 2], b[BOX_CH+1];
    const char *pf = G.hrx[i] ? "" : "TX ";
    uint8_t pl = slen(pf), n = 0, c = 0;
    for(uint8_t j = 0; j < pl; j++) full[n++] = pf[j];
    for(uint8_t j = 0; j < G.hlen[i]; j++) full[n++] = G.hist[i][j];
    uint8_t off = (uint8_t)(k * BOX_CH);
    for(uint8_t j = 0; j < BOX_CH && (uint8_t)(off + j) < n; j++) b[c++] = full[off + j];
    b[c] = 0;
    if(G.hrx[i]) A->print_bold(b, 0, 0, row);
    else A->print_normal(b, 0, 0, row);
}
static void drawBox(void){
    uint8_t total = boxTotalLines();
    uint8_t maxScroll = (total > BOX_LINES) ? (uint8_t)(total - BOX_LINES) : 0;
    if(G.scroll > maxScroll) G.scroll = maxScroll;
    int16_t start = (int16_t)total - BOX_LINES - (int16_t)G.scroll;
    if(start < 0) start = 0;
    uint8_t lc = 0, r = 0;
    for(uint8_t i = 0; i < G.hn && r < BOX_LINES; i++){
        uint8_t ml = msgLines(i);
        for(uint8_t k = 0; k < ml; k++){
            if(lc >= (uint8_t)start){ drawBoxLine(i, k, (uint8_t)(BOX_TOP + r)); r++; }
            lc++;
            if(r >= BOX_LINES) break;
        }
    }
}

/* ---- bottom status row (line 6): WPM and SQL on the left, frequency right ---- */
static void drawStatus(void){
    char b[19], *p;
    uint32_t rem;
    b[0] = 'W'; p = b + 1; p = pnum(p, udiv(1200u, G.unitMs, NULL), 1);
    *p++ = ' '; *p++ = 'S'; p = pnum(p, G.sqlLevel, 1);
    A->print_normal(b, 2, 0, 6);
    p = pnum(b, udiv(A->rx_freq(), 100000u, &rem), 1);
    *p++ = '.'; p = pnum(p, rem, 5);
    A->print_normal(b, (uint8_t)(126 - (p - b) * 7), 0, 6);
}

/* ---- live TX/RX preview (line 0) ----
 * RECV blinks while a signal is actually being received (squelch open) and
 * stands steady the rest of the time; the dot/dash decode and the counters sit
 * next to it as before. */
static void drawPreview(void){
    char b[19], t[6], *p, *q;
    if(G.txPos){
        p = b; p = pstr(p, "SEND ");
        for(uint8_t i = 0; G.txPat[i]; i++) *p++ = G.txPat[i];
        *p = 0;
        q = t; q = pnum(q, G.txPos, 1); *q++ = '/'; q = pnum(q, G.msgLen, 1);
    } else {
        p = b; p = pstr(p, ((G.flags & F_SQL) && G.blink) ? "     " : "RECV ");
        uint8_t bit = 0x80; while(bit > 1 && !(G.rxCode & bit)) bit >>= 1;
        for(bit >>= 1; bit; bit >>= 1) *p++ = (G.rxCode & bit) ? '-' : '.';
        *p = 0;
        q = t; q = pnum(q, G.rxCount, 1);
    }
    A->print_normal(b, 0, 0, 0);
    A->print_normal(t, (uint8_t)(126 - (q - t) * 7), 0, 0);
    for(uint8_t x = 0; x < 128; x++) A->fb[0][x] ^= 0xFF;
}

/* Settings page: UP/DOWN selects a row, MENU enters and stores its value,
 * UP/DOWN then adjusts it (bracketed while editing), EXIT saves. */
static void drawMenu(void){
    char b[19], *p;
    for(uint8_t row = 1; row <= 2; row++){
        uint8_t ed = (uint8_t)(G.edit && G.menu == row);
        p = b; *p++ = (G.menu == row) ? '>' : ' ';
        p = pstr(p, row == 1 ? "WPM " : "SQL ");
        if(ed) *p++ = '[';
        p = pnum(p, row == 1 ? udiv(1200u, G.unitMs, NULL) : G.sqlLevel, 1);
        if(ed) *p++ = ']';
        *p = 0;
        A->print_normal(b, 2, 0, row);
    }
}

static void draw(void){
    char b[19], r[8], *q;
    chrome();
    drawPreview();
    if(G.menu){ drawMenu(); drawStatus(); return; }
    drawBox();
    uint8_t hint = G.msgLen < MSG_MAX ? (uint8_t)(MSG_MAX - G.msgLen) : 0;
    q = r; *q++ = '('; q = pnum(q, hint, 1); *q++ = ')'; *q = 0;
    uint8_t hw = (uint8_t)((q - r) + 1);
    uint8_t maxc = (uint8_t)(18 - hw);
    uint8_t st = (G.msgLen > (uint8_t)(maxc - 1)) ? (uint8_t)(G.msgLen - (maxc - 1)) : 0;
    uint8_t c = 0;
    b[c++] = '>';
    for(uint8_t i = 0; i < (uint8_t)(maxc - 1) && (uint8_t)(st + i) < G.msgLen; i++) b[c++] = G.msg[st + i];
    if(G.blink) b[c++] = '_';
    b[c] = 0;
    /* The input box doubles as the status box: a timed popup wins, and while a
     * signal is coming in (squelch open, so nothing can be sent) it reads BUSY
     * instead of the compose text - the text itself is kept and comes back as
     * soon as the channel clears. */
    if(G.popT || (G.flags & F_SQL)){
        char *z = (char*)getFlatStr(POP_FLAT, G.popT ? G.pop : 0);
        c = 0; while(*z) b[c++] = *z++; b[c] = 0;
    }
    A->print_normal(b, 0, 0, ROW_IN);
    A->print_normal(r, (uint8_t)(126 - (q - r) * 7), 0, ROW_IN);
    for(uint8_t x = 0; x < 128; x++) A->fb[ROW_IN][x] ^= 0xFF;
    drawStatus();
}

static void drawAndBlit(void){ draw(); blitAll(); }

static void redLed(bool on){
    uint16_t v = A->bk_read(GPIO_REG) & ~1u;
    A->bk_write(GPIO_REG, on ? (uint16_t)(v | 1u) : v);
}
static void keyOn(void){
    A->tx_mute(false);
    if(!(G.flags & F_TONE)) A->tx_carrier(true);
    redLed(true);
}
static void keyOff(void){
    A->tx_mute(true);
    if(!(G.flags & F_TONE)) A->tx_carrier(false);
    redLed(false);
}

static uint8_t sendDelay(uint16_t ms){
    while(ms){
        uint16_t sl = (ms > 10u) ? 10u : ms;
        A->delay_ms(sl); ms -= sl;
        uint8_t k = A->get_key();
        if(k == APP_KEY_EXIT) return 1;
        if(k == APP_KEY_PTT) return 1;
    }
    return 0;
}
static bool sendChar(char c){
    uint8_t code = morseByte(c);
    uint8_t n = 0; G.txPat[0] = 0;
    if(code == 0) return sendDelay((uint16_t)(G.unitMs * 4u)) != 0;
    uint32_t bit = 0x80;
    while(!(code & bit)) bit >>= 1;
    for(bit >>= 1; bit; bit >>= 1){
        G.txPat[n++] = (code & bit) ? '-' : '.'; G.txPat[n] = 0;
        drawAndBlit();
        keyOn();
        if(sendDelay((code & bit) ? (uint16_t)(G.unitMs * 3u) : G.unitMs)){ keyOff(); return true; }
        keyOff();
        if(sendDelay(G.unitMs)) return true;
    }
    return sendDelay((uint16_t)(G.unitMs * 2u)) != 0;
}

static void drainKeys(void){
    while(A->get_key() != APP_KEY_INVALID) A->delay_ms(10);
    G.prevKey = APP_KEY_INVALID;
}
static void rxSample(void);
static void rxTune(bool full);

static bool sendOnce(void){
    A->tx_set_params();
    A->tx_tone(TONE_HZ); A->set_af(AF_BEEP); A->audio_path(true); A->tx_mute(true);
    if(G.flags & F_TONE) A->tx_carrier(true);
    histNew(false);
    G.rxCount = 0; G.txPos = 0;
    bool aborted = false;
    for(uint8_t i = 0; i < G.msgLen; i++){
        G.txPos = (uint8_t)(i + 1);
        if(sendChar(G.msg[i])){ aborted = true; break; }
        histApp(G.msg[i]);
        drawAndBlit();
    }
    G.txPat[0] = 0; G.txPos = 0;
    A->tx_mute(true); A->tx_carrier(false);
    A->audio_path(false); A->tx_end();
    return !aborted;
}
static void sendMessage(void){
    if(G.msgLen == 0){ if(!(G.flags & F_SQL)) beepDeny(); return; }
    if(G.flags & F_SQL){ alert(0); return; }
    if(sendOnce()){ G.msgLen = 0; G.msg[0] = 0; }
    G.scroll = 0;
    drainKeys(); rxTune(false); drawAndBlit();
}

static void rxPush(char c){
    if(!c) return;
    if(c != ' ' && G.rxCount < 255) G.rxCount++;
    if(c == ' '){ if(G.rxNew) return; histApp(c); return; }
    if(G.rxNew){ histNew(true); G.rxNew = 0; }
    histApp(c);
}
static void rxFlush(void){
    if(G.flags & F_PEND){ rxPush(morseChar(G.rxCode)); G.rxCode = 1; G.flags &= ~F_PEND; }
}

static void rxSample(void){
    int16_t raw = A->rssi_dbm();
    if(G.blank){
        G.blank--;
        G.smooth_rssi = raw; G.rxFloor = raw; G.rxPeak = raw; G.sqlFloor = raw;
        G.flags &= ~(F_MARK | F_PEND | F_SQL); G.rxRun = 0; G.rxCode = 1;
        if(G.sinceChar < 60000u) G.sinceChar++;
        A->set_af(APP_AF_MUTE); A->led(false);
        return;
    }
    G.smooth_rssi = (int16_t)(G.smooth_rssi + ((raw - G.smooth_rssi) >> 1));
    int16_t r = G.smooth_rssi;
    bool curr_mark = ((G.flags & F_MARK) != 0);

    if(!curr_mark) G.sqlFloor += (int16_t)((r - G.sqlFloor) >> 4);
    int16_t open = (int16_t)(6 + (G.sqlLevel << 1));
    if(G.sqlLevel == 0){ if(!(G.flags & F_SQL)){ G.flags |= F_SQL; A->set_af(APP_AF_FM); } }
    else if(!(G.flags & F_SQL)){ if(r >= G.sqlFloor + open){ G.flags |= F_SQL; A->set_af(APP_AF_FM); } }
    else if(r < G.sqlFloor + open - 6){ G.flags &= ~F_SQL; A->set_af(APP_AF_MUTE); }

    uint16_t burstGap = (uint16_t)(G.unitMs * 3u / 2u);
    if(G.flags & F_SQL){
        if(G.sinceChar >= burstGap){ G.firstMark = 1; G.rxCode = 1; G.flags &= ~F_PEND; G.rxNew = 1; G.rxCount = 0; histPrune(); }
        G.sinceChar = 0;
    }
    else {
        if(G.sinceChar < 60000u) G.sinceChar++;
        if(G.sinceChar == burstGap) histPrune();   /* burst went quiet: drop it if it was only T/E */
    }

    if(r < G.rxFloor) G.rxFloor = r;
    if(r > G.rxPeak) G.rxPeak = r;
    if(++G.rxAdapt >= 16){
        G.rxAdapt = 0;
        if(G.rxFloor < r) G.rxFloor++;
        if(G.rxPeak > r) G.rxPeak--;
    }
    if(G.rxPeak < (int16_t)(G.rxFloor + 14)) G.rxPeak = (int16_t)(G.rxFloor + 14);

    int16_t range = G.rxPeak - G.rxFloor;
    int16_t third = (int16_t)udiv((uint32_t)range, 3u, NULL);
    int16_t th_high = G.rxFloor + (third << 1);
    int16_t th_low = G.rxFloor + third;

    bool mark = curr_mark;
    if(!curr_mark){ if((G.flags & F_SQL) && r > th_high) mark = true; }
    else if(r < th_low) mark = false;

    if(mark == curr_mark){
        if(G.rxRun < 60000u) G.rxRun += RX_TICK;
        if((G.flags & F_PEND) && (G.rxRun >= 60000u || (!curr_mark && G.rxRun >= (uint16_t)(G.unitMs * 2u)))) rxFlush();
        return;
    }

    A->led(mark);
    uint16_t d = G.rxRun; G.rxRun = 0;
    if(curr_mark){
        if(G.firstMark){ d = (d > G.unitMs) ? (uint16_t)(d - G.unitMs) : 0; G.firstMark = 0; }
        G.rxCode = (uint8_t)((G.rxCode << 1) | ((d >= (uint16_t)(G.unitMs * 2u)) ? 1u : 0u));
        G.flags |= F_PEND;
        if(G.rxCode > 0x7F){ rxPush('?'); G.rxCode = 1; G.flags &= ~F_PEND; }
    } else {
        if(d >= (uint16_t)(G.unitMs * 5u)){ rxFlush(); rxPush(' '); }
        else if(d >= (uint16_t)(G.unitMs * 2u)) rxFlush();
    }
    if(mark) G.flags |= F_MARK; else G.flags &= ~F_MARK;
}

static void rxTune(bool full){
    A->set_agc(false); A->set_af(APP_AF_MUTE); A->audio_path(true);
    A->delay_ms(50);
    int16_t live = A->rssi_dbm();
    G.flags = (uint8_t)(G.flags & ~(F_MARK | F_PEND | F_SQL));
    G.rxCode = 1; G.rxRun = 0; G.rxAdapt = 0;
    G.smooth_rssi = live; G.rxFloor = live; G.rxPeak = live; G.sqlFloor = live;
    G.rxNew = 0; G.sinceChar = 0; G.txPos = 0; G.txPat[0] = 0;
    G.blank = full ? 0 : 48;
    if(!full) G.firstMark = 1;
}

/* ---- input ---- */
static void handleTap(uint8_t key){
    if(key == APP_KEY_0){
        if(G.msgLen > 0) G.msg[--G.msgLen] = 0;
        G.lastKey = APP_KEY_INVALID; G.tapMs = 0;
    } else if(key == APP_KEY_STAR){
        if(G.msgLen < MSG_MAX){ G.msg[G.msgLen++] = ' '; G.msg[G.msgLen] = 0; }
        else alert(1);
        G.lastKey = APP_KEY_INVALID; G.tapMs = 0;
    } else if(key <= APP_KEY_9){
        const char *set = KEYMAP[key];
        if(set[0]){
            if(key == G.lastKey && G.tapMs > 0 && G.msgLen > 0){
                G.tapIdx++; if(!set[G.tapIdx]) G.tapIdx = 0;
                G.msg[G.msgLen - 1] = set[G.tapIdx];
            } else if(G.msgLen < MSG_MAX){
                G.tapIdx = 0; G.msg[G.msgLen++] = set[0]; G.msg[G.msgLen] = 0;
            } else alert(1);
            G.lastKey = key; G.tapMs = TAP_MS;
        }
    }
}

static void holdTick(void){
    uint8_t key = A->get_key();
    if(key == APP_KEY_F){
        if(G.menu) G.fHoldMs = 0;   /* F gestures belong to the home screen */
        else if(G.fHoldMs < HOLD_MS + TICK_MS) G.fHoldMs += TICK_MS;
        if(G.fHoldMs == HOLD_MS){ G.flags ^= F_TONE; G.flags &= ~F_FDOWN; G.fTapMs = 0; alert((G.flags & F_TONE) ? 2 : 3); drawAndBlit(); }
    } else {
        if(G.fHoldMs > 0){
            if(G.fHoldMs < HOLD_MS){
                if(G.fTapMs > 0){
                    G.msgLen = 0; G.msg[0] = 0;      /* clear input */
                    G.hn = 0; G.scroll = 0; G.rxNew = 0;   /* clear the message box too */
                    G.flags &= ~F_FDOWN; G.fTapMs = 0; G.lastKey = APP_KEY_INVALID; G.tapMs = 0;
                } else { G.flags ^= F_FDOWN; G.fTapMs = 400; }
                drawAndBlit();
            }
            G.fHoldMs = 0;
        }
    }
    if(key == APP_KEY_PTT){
        if(!G.menu && G.pttHoldMs < HOLD_MS + TICK_MS) G.pttHoldMs += TICK_MS;
    } else {
        if(G.pttHoldMs > 0 && G.pttHoldMs < HOLD_MS && !G.menu){ sendMessage(); drainKeys(); drawAndBlit(); }
        G.pttHoldMs = 0;
    }
}

static void wpmAdjust(int8_t dir){
    if(dir > 0){ if(G.unitMs > UNIT_MIN) G.unitMs -= UNIT_STEP; }
    else if(G.unitMs < UNIT_MAX) G.unitMs += UNIT_STEP;
}
/* Directional message-box scroll: UP shows older entries, DOWN newer ones.
 * Plain UP/DOWN scrolls; WPM speed moved to F+UP/DOWN (see app_main). */
static void scrollBox(int8_t dir){
    uint8_t total = boxTotalLines();
    uint8_t maxScroll = (total > BOX_LINES) ? (uint8_t)(total - BOX_LINES) : 0;
    if(dir > 0){ if(G.scroll < maxScroll) G.scroll++; }   /* UP: older */
    else if(G.scroll > 0) G.scroll--;                     /* DOWN: newer */
}
static void sqlAdjust(int8_t dir){
    uint8_t old = G.sqlLevel;
    if(dir > 0){ if(G.sqlLevel < 9) G.sqlLevel++; } else if(G.sqlLevel > 0) G.sqlLevel--;
    if(G.sqlLevel != old){ G.flags &= ~F_SQL; A->set_af(APP_AF_MUTE); }
}

static void loadConfig(void){
    uint8_t c[8]; A->cfg_load(c, 8);
    if(c[0] == CFG_MAGIC){
        uint16_t u = c[1] | (c[2] << 8);
        if(u >= UNIT_MIN && u <= UNIT_MAX) G.unitMs = u;
        if(c[3] <= 9) G.sqlLevel = c[3];
        if(c[4]) G.flags |= F_TONE;
    }
}
static void saveConfig(void){
    uint8_t c[8] = {CFG_MAGIC, G.unitMs & 0xFF, G.unitMs >> 8, G.sqlLevel, (G.flags & F_TONE) ? 1 : 0, 0, 0, 0};
    A->cfg_save(c, 8);
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api){
    uint8_t rxAcc = 0;
    A = api;

    memset(&G, 0, sizeof(G));
    G.lastKey = APP_KEY_INVALID;
    G.prevKey = APP_KEY_INVALID;
    G.unitMs = UNIT_DEF;
    G.sqlLevel = SQL_DEF;
    G.flags = F_RUN;

    loadConfig();
    rxTune(true);
    A->backlight_on();
    drawAndBlit();

    while(G.flags & F_RUN){
        uint8_t key = A->get_key();

        if(key == APP_KEY_EXIT && key != G.prevKey){
            if(G.menu){ G.menu = 0; saveConfig(); G.prevKey = key; drawAndBlit(); continue; }
            if(G.exArm){ G.flags &= ~F_RUN; G.prevKey = key; continue; }   /* second press quits */
            G.exArm = EXIT_TICKS; alert(4); G.prevKey = key; drawAndBlit(); continue;
        }

        if((G.flags & F_FDOWN) && key != G.prevKey && key != APP_KEY_INVALID && key != APP_KEY_F){
            G.flags &= ~F_FDOWN; G.fTapMs = 0; G.prevKey = key; A->backlight_on();
            int8_t fdir = A->nav_dir(key);
            if(fdir) wpmAdjust(fdir);
            else if(key >= APP_KEY_1 && key <= APP_KEY_9){
                const char *s = PROSIGN_T[key];
                for(uint8_t i = 0; s[i]; i++)
                    if(G.msgLen < MSG_MAX){ G.msg[G.msgLen++] = s[i]; G.msg[G.msgLen] = 0; }
            }
            drawAndBlit();
        } else if(key != APP_KEY_INVALID && key != G.prevKey && key != APP_KEY_F && key != APP_KEY_PTT){
            G.prevKey = key; A->backlight_on();
            int8_t dir = A->nav_dir(key);
            if(G.menu){
                if(dir){
                    if(G.edit){ if(G.menu == 1) wpmAdjust(dir); else sqlAdjust(dir); }
                    else G.menu = (G.menu == 1) ? 2u : 1u;   /* UP/DOWN picks the row */
                } else if(key == APP_KEY_MENU) G.edit ^= 1u; /* MENU enters/stores the value */
            }
            else if(dir) scrollBox(dir);
            else if(key == APP_KEY_MENU){ G.menu = 1; G.edit = 0; G.fHoldMs = 0; G.lastKey = APP_KEY_INVALID; G.tapMs = 0; }
            else if(key == APP_KEY_SIDE1) sqlAdjust(1);
            else if(key == APP_KEY_SIDE2) sqlAdjust(-1);
            else handleTap(key);
            if(G.flags & F_RUN) drawAndBlit();
        } else if(key != APP_KEY_F && key != APP_KEY_PTT){
            G.prevKey = key;
        }
        holdTick();

        if(G.tapMs > TICK_MS) G.tapMs -= TICK_MS; else if(G.tapMs){ G.tapMs = 0; G.lastKey = APP_KEY_INVALID; }
        if(G.fTapMs > TICK_MS) G.fTapMs -= TICK_MS; else if(G.fTapMs) G.fTapMs = 0;
        if(G.exArm) G.exArm--;

        for(int i = 0; i < TICK_MS / RX_TICK; i++){ rxSample(); A->delay_ms(RX_TICK); }
        if(G.popT && --G.popT == 0){ G.pop = 0; drawAndBlit(); }
        if(++rxAcc >= 8){ rxAcc = 0; G.blink ^= 1; drawAndBlit(); }

        if(!(G.flags & F_MARK) && ++G.batTimer >= 40){ G.batTimer = 0; A->battery_sample(); }
        A->backlight_update();
    }

    saveConfig();
    A->set_agc(true);
    A->audio_path(false);
    A->led(false);
    A->tx_end();
}
