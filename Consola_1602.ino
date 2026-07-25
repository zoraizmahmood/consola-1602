/* =============================================================
   CONSOLA 1602  -  Arduino Uno R3 + LCD 16x2 paralelo
   5 botones + buzzer activo. Sin RTC (reloj por software).

   v1.1: reloj, Snake, Dino, Simon, reflejos, Pomodoro, notas,
   sistema y ajustes. Cada aplicacion es un modulo con
   enter()/loop() y el bucle principal no bloquea nunca.

   ---------------------------------------------------------
   CONEXIONES
   ---------------------------------------------------------
   LCD 1602 (modo 4 bits)
     VSS -> GND         VDD -> 5V
     V0  -> patilla central de un potenciometro de 10K
            (los otros dos extremos del pot a 5V y GND)
     RS  -> D12         RW  -> GND        E -> D11
     D4  -> D5   D5 -> D4   D6 -> D3   D7 -> D2
     A (15) -> ver nota de retroiluminacion
     K (16) -> GND

   Botones (el otro extremo de cada uno a GND, sin resistencias:
   usamos los pull-up internos)
     ARRIBA    -> D6
     ABAJO     -> D7
     IZQUIERDA -> D10
     DERECHA   -> A0
     OK        -> A1

   Buzzer activo
     + -> D8      - -> GND

   Retroiluminacion (opcional, ver USE_BACKLIGHT_PIN abajo)
     A(15) -> resistencia 220 ohm -> D9
     Si prefieres no controlarla: A(15) -> 220 ohm -> 5V
     y pon USE_BACKLIGHT_PIN a 0.

   LIBRES para el futuro: D13, A2, A3, A4(SDA), A5(SCL)
   -> A4/A5 se dejan libres a proposito para poder anadir
      un DS3231 mas adelante sin tocar el cableado.
   ---------------------------------------------------------
   NOTA: la ROM del HD44780 no tiene tildes ni ñ. Todos los
   textos van sin acentos a proposito.
   ============================================================= */

#include <LiquidCrystal.h>
#include <EEPROM.h>

/* ================= CONFIGURACION DE PINES ================= */
LiquidCrystal lcd(12, 11, 5, 4, 3, 2);   // RS, E, D4, D5, D6, D7

#define PIN_BUZZER        8
#define PASSIVE_BUZZER    0   // 1 si cambias a un zumbador pasivo:
                              // Simon pasa de ritmos a notas
#define PIN_BACKLIGHT     9
#define USE_BACKLIGHT_PIN 1              // 0 = retro siempre fija a 5V

/* ===================== BOTONES ============================ */
enum { B_UP = 0, B_DOWN, B_LEFT, B_RIGHT, B_OK, B_COUNT };
const uint8_t BTN_PIN[B_COUNT] = { 6, 7, 10, A0, A1 };

const uint16_t T_DEBOUNCE  = 25;    // ms de antirrebote
const uint16_t T_LONG      = 600;   // ms para pulsacion larga
const uint16_t T_REP_START = 450;   // ms antes de auto-repetir
const uint16_t T_REP       = 110;   // ms entre repeticiones

// Mascaras de eventos de este ciclo (bit i = boton i)
uint8_t bHeld = 0, bPressed = 0, bClick = 0, bLong = 0, bRepeat = 0;
bool    bIgnore = false;            // ignora todo hasta soltar

struct BtnState {
  bool     raw, stable, longDone;
  uint32_t tEdge, tDown, tNextRep;
};
BtnState B[B_COUNT];

inline bool held  (uint8_t b) { return bHeld    & (1 << b); }
inline bool press (uint8_t b) { return bPressed & (1 << b); }
inline bool click (uint8_t b) { return bClick   & (1 << b); }
inline bool longP (uint8_t b) { return bLong    & (1 << b); }
inline bool rep   (uint8_t b) { return (bPressed | bRepeat) & (1 << b); }

void backlightWake();

void buttonsUpdate() {
  uint32_t now = millis();
  bPressed = bClick = bLong = bRepeat = 0;

  for (uint8_t i = 0; i < B_COUNT; i++) {
    bool r = (digitalRead(BTN_PIN[i]) == LOW);
    if (r != B[i].raw) { B[i].raw = r; B[i].tEdge = now; }

    if (now - B[i].tEdge >= T_DEBOUNCE && B[i].stable != B[i].raw) {
      B[i].stable = B[i].raw;
      if (B[i].stable) {                       // flanco de pulsacion
        B[i].tDown = now;
        B[i].longDone = false;
        B[i].tNextRep = now + T_REP_START;
        bPressed |= (1 << i);
        bHeld    |= (1 << i);
      } else {                                 // flanco de suelta
        bHeld &= ~(1 << i);
        if (!B[i].longDone) bClick |= (1 << i); // click = pulsacion corta
      }
    }

    if (B[i].stable) {
      if (!B[i].longDone && now - B[i].tDown >= T_LONG) {
        B[i].longDone = true;
        bLong |= (1 << i);
      }
      if ((int32_t)(now - B[i].tNextRep) >= 0) {
        B[i].tNextRep = now + T_REP;
        bRepeat |= (1 << i);
      }
    }
  }

  // Tras cambiar de app ignoramos eventos hasta soltar todos los
  // botones. Evita el clasico "entro en la app y se sale sola".
  if (bIgnore) {
    if (bHeld == 0) bIgnore = false;
    bPressed = bClick = bLong = bRepeat = 0;
  }

  if (bPressed | bClick | bLong) backlightWake();
}

/* ============ BUZZER ACTIVO (patrones no bloqueantes) ===== */
uint8_t  beepLeft = 0;
bool     beepOn = false;
uint32_t beepT = 0;
uint16_t beepOnMs = 60, beepOffMs = 80;

struct Cfg;                 // fwd
bool isMuted();

void beep(uint8_t n, uint16_t on, uint16_t off) {
  if (isMuted() || n == 0) return;
  beepLeft = n; beepOnMs = on; beepOffMs = off;
  beepOn = true; beepT = millis();
  digitalWrite(PIN_BUZZER, HIGH);
}

void beepUpdate() {
  if (beepLeft == 0) return;
  uint32_t now = millis();
  if (beepOn && now - beepT >= beepOnMs) {
    digitalWrite(PIN_BUZZER, LOW);
    beepOn = false; beepT = now; beepLeft--;
  } else if (!beepOn && beepLeft > 0 && now - beepT >= beepOffMs) {
    digitalWrite(PIN_BUZZER, HIGH);
    beepOn = true; beepT = now;
  }
}

// Vocabulario sonoro: cada evento con su ritmo propio, para
// reconocerlo sin mirar la pantalla.
inline void sndMove()  { beep(1,  25,  40); }
inline void sndOk()    { beep(2,  40,  60); }
inline void sndBack()  { beep(1,  90,   0); }
inline void sndError() { beep(1, 300,   0); }
inline void sndAlarm() { beep(3, 250, 150); }

/* ================ AJUSTES EN EEPROM ======================= */
#define CFG_MAGIC   0xC1
#define CFG_VERSION 4
#define CFG_ADDR    0

struct Cfg {
  uint8_t  magic, version;
  uint8_t  mute;          // 0/1
  uint8_t  brightness;    // 0-255 (PWM retro)
  uint16_t blTimeout;     // segundos, 0 = nunca apagar
  int16_t  calib;         // correccion del reloj en ms por hora
  uint16_t snakeHi;       // record Snake
  uint16_t reflexBest;    // mejor tiempo de reflejos (ms)
  uint8_t  pomoWork, pomoBreak, pomoCycles;
  int8_t   tempOffset;    // calibracion del sensor interno
  uint16_t dinoHi;        // record del Dino
  uint8_t  simonHi;       // secuencia mas larga en Simon
};
Cfg cfg;

bool isMuted() { return cfg.mute; }

void cfgDefaults() {
  cfg.magic = CFG_MAGIC; cfg.version = CFG_VERSION;
  // OJO: blTimeout = 0 (nunca apagar) hasta que los botones esten
  // cableados. Si se apaga y no hay botones, no hay forma de
  // despertarla y parece que el LCD esta muerto.
  cfg.mute = 0; cfg.brightness = 100; cfg.blTimeout = 0;
  cfg.calib = 0; cfg.snakeHi = 0; cfg.reflexBest = 0;
  cfg.pomoWork = 25; cfg.pomoBreak = 5; cfg.pomoCycles = 4;
  cfg.tempOffset = 0; cfg.dinoHi = 0; cfg.simonHi = 0;
}
void cfgLoad() {
  EEPROM.get(CFG_ADDR, cfg);
  if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION) {
    cfgDefaults();
    EEPROM.put(CFG_ADDR, cfg);
  }
}
void cfgSave() { EEPROM.put(CFG_ADDR, cfg); }   // EEPROM.put ya
                                                // evita reescribir
                                                // bytes iguales

/* ================ RETROILUMINACION ======================== */
bool     blOn = true;
uint32_t blLast = 0;

void backlightApply(uint8_t v) {
#if USE_BACKLIGHT_PIN
  analogWrite(PIN_BACKLIGHT, v);
#else
  (void)v;
#endif
}
void backlightWake() {
  blLast = millis();
  if (!blOn) { blOn = true; backlightApply(cfg.brightness); }
}
void backlightUpdate() {
  if (blOn && cfg.blTimeout > 0 &&
      millis() - blLast > (uint32_t)cfg.blTimeout * 1000UL) {
    blOn = false; backlightApply(0);
  }
}

/* ============ RELOJ POR SOFTWARE (sin RTC) ================
   Cuenta segundos desde medianoche usando millis(). Se pone en
   hora a mano y se pierde al desenchufar: es el precio de no
   tener DS3231. cfg.calib compensa la deriva del cristal:
   si en 24 h se adelanta 6 s -> 6000/24 = 250 -> calib = -250.
   =========================================================== */
uint32_t clkLastMs = 0;
uint32_t clkSec    = 12UL * 3600UL;   // arranca a las 12:00
int32_t  clkAcc    = 0;               // acumulador de ms

void clockUpdate() {
  uint32_t now = millis();
  clkAcc += (int32_t)(now - clkLastMs);
  clkLastMs = now;
  while (clkAcc >= 1000) {
    clkAcc -= 1000;
    clkSec++;
    if (clkSec >= 86400UL) clkSec = 0;
    if (clkSec % 3600UL == 0) clkAcc += cfg.calib;   // deriva
  }
}
inline uint8_t clkH() { return clkSec / 3600UL; }
inline uint8_t clkM() { return (clkSec / 60UL) % 60UL; }
inline uint8_t clkS() { return clkSec % 60UL; }

/* ============ FUENTE GRANDE (8 chars a medida) ============ */
const uint8_t GLYPHS[8][8] PROGMEM = {
  {B00111,B01111,B11111,B11111,B11111,B11111,B11111,B11111}, // 0 LT
  {B11111,B11111,B11111,B00000,B00000,B00000,B00000,B00000}, // 1 UB
  {B11100,B11110,B11111,B11111,B11111,B11111,B11111,B11111}, // 2 RT
  {B11111,B11111,B11111,B11111,B11111,B11111,B01111,B00111}, // 3 LL
  {B00000,B00000,B00000,B00000,B00000,B11111,B11111,B11111}, // 4 LB
  {B11111,B11111,B11111,B11111,B11111,B11111,B11110,B11100}, // 5 LR
  {B11111,B11111,B11111,B00000,B00000,B00000,B11111,B11111}, // 6 UMB
  {B11111,B00000,B00000,B00000,B00000,B11111,B11111,B11111}  // 7 LMB
};
#define LT 0
#define UB 1
#define RT 2
#define LL 3
#define LB 4
#define LR 5
#define UMB 6
#define LMB 7
#define BLK 255
#define SPC 32

void loadGlyphs() {
  uint8_t tmp[8];
  for (uint8_t i = 0; i < 8; i++) {
    memcpy_P(tmp, GLYPHS[i], 8);
    lcd.createChar(i, tmp);
  }
}
inline void wr(uint8_t c) { lcd.write(c); }

// Dibuja un digito grande (3 columnas de ancho, 2 filas de alto)
void bigDigit(uint8_t n, uint8_t col) {
  const uint8_t top[10][3] = {
    {LT,UB,RT},{UB,RT,SPC},{UMB,UMB,RT},{UMB,UMB,RT},{LL,LB,BLK},
    {BLK,UMB,UMB},{LT,UMB,UMB},{UB,UB,RT},{LT,UMB,RT},{LT,UMB,RT}
  };
  const uint8_t bot[10][3] = {
    {LL,LB,LR},{SPC,BLK,SPC},{LL,LMB,LMB},{LMB,LMB,LR},{SPC,SPC,BLK},
    {LMB,LMB,LR},{LL,LMB,LR},{SPC,SPC,BLK},{LL,LMB,LR},{LMB,LMB,LR}
  };
  if (n > 9) return;
  lcd.setCursor(col, 0); for (uint8_t i=0;i<3;i++) wr(top[n][i]);
  lcd.setCursor(col, 1); for (uint8_t i=0;i<3;i++) wr(bot[n][i]);
}

/* =================== UTILIDADES LCD ======================= */
void clearRow(uint8_t row) {
  lcd.setCursor(0, row);
  for (uint8_t i = 0; i < 16; i++) lcd.print(' ');
}
void printAt(uint8_t col, uint8_t row, const __FlashStringHelper *s) {
  lcd.setCursor(col, row); lcd.print(s);
}
void printCentered(uint8_t row, const char *s) {
  uint8_t len = strlen(s);
  if (len > 16) len = 16;
  uint8_t pad = (16 - len) / 2;
  clearRow(row);
  lcd.setCursor(pad, row);
  for (uint8_t i = 0; i < len; i++) lcd.print(s[i]);
}
void print2(uint8_t v) { if (v < 10) lcd.print('0'); lcd.print(v); }

/* ============ MAQUINA DE ESTADOS / APPS ===================
   Cada app implementa enter() (dibuja de cero, inicializa) y
   loop() (se llama cada ciclo, sin bloquear nunca). Se sale
   siempre con OK largo. Anadir una app = escribir dos funciones
   y una linea en las tablas del final.
   =========================================================== */
enum AppId {
  APP_HOME = 0, APP_MENU, APP_SNAKE, APP_DINO, APP_SIMON,
  APP_REFLEX, APP_POMO, APP_NOTES, APP_STATS, APP_SETTINGS,
  APP_COUNT
};

AppId    appCur = APP_HOME;
AppId    appPrev = APP_HOME;
uint32_t appEnterMs = 0;

void switchTo(AppId id);

/* ---------------------- HOME (reloj) ---------------------- */
uint8_t  homeLastM = 255;
uint8_t  homeLastS = 255;
bool     homeColon = true;

void home_enter() {
  loadGlyphs();                 // Snake sobrescribe la CGRAM
  lcd.clear();
  homeLastM = 255; homeLastS = 255;
}
void home_loop() {
  uint8_t h = clkH(), m = clkM(), s = clkS();

  if (m != homeLastM) {           // solo redibujamos si cambia
    homeLastM = m;
    bigDigit(h / 10, 0);
    bigDigit(h % 10, 3);
    bigDigit(m / 10, 7);
    bigDigit(m % 10, 10);
  }
  if (s != homeLastS) {
    homeLastS = s;
    homeColon = !homeColon;
    lcd.setCursor(6, 0); lcd.print(homeColon ? '.' : ' ');
    lcd.setCursor(6, 1); lcd.print(homeColon ? '.' : ' ');
    lcd.setCursor(13, 1); lcd.print(':'); print2(s);
    lcd.setCursor(13, 0);
    if (cfg.mute) lcd.print(F("MUT")); else lcd.print(F("   "));
  }

  if (click(B_OK)) { sndOk(); switchTo(APP_MENU); }
}

/* ------------------------ MENU ---------------------------- */
const char mnu0[] PROGMEM = "Snake";
const char mnu1[] PROGMEM = "Dino";
const char mnu2[] PROGMEM = "Simon";
const char mnu3[] PROGMEM = "Reflejos";
const char mnu4[] PROGMEM = "Pomodoro";
const char mnu5[] PROGMEM = "Notas";
const char mnu6[] PROGMEM = "Sistema";
const char mnu7[] PROGMEM = "Ajustes";
const char mnu8[] PROGMEM = "Reloj";

struct MenuItem { PGM_P name; uint8_t app; };
const MenuItem MENU[] = {
  { mnu0, APP_SNAKE  }, { mnu1, APP_DINO     },
  { mnu2, APP_SIMON  }, { mnu3, APP_REFLEX   },
  { mnu4, APP_POMO   }, { mnu5, APP_NOTES    },
  { mnu6, APP_STATS  }, { mnu7, APP_SETTINGS },
  { mnu8, APP_HOME   }
};
const uint8_t MENU_N = sizeof(MENU) / sizeof(MENU[0]);

uint8_t menuSel = 0, menuTop = 0;
bool    menuDirty = true;

void menu_draw() {
  char buf[17];
  for (uint8_t r = 0; r < 2; r++) {
    uint8_t idx = menuTop + r;
    clearRow(r);
    lcd.setCursor(0, r);
    if (idx < MENU_N) {
      lcd.print(idx == menuSel ? '>' : ' ');
      strncpy_P(buf, MENU[idx].name, 16);
      buf[15] = '\0';
      lcd.print(buf);
    }
  }
  menuDirty = false;
}
void menu_enter() { lcd.clear(); menuDirty = true; }
void menu_loop() {
  if (rep(B_UP) && menuSel > 0)            { menuSel--; sndMove(); menuDirty = true; }
  if (rep(B_DOWN) && menuSel < MENU_N - 1) { menuSel++; sndMove(); menuDirty = true; }

  if (menuSel < menuTop)     menuTop = menuSel;
  if (menuSel > menuTop + 1) menuTop = menuSel - 1;

  if (menuDirty) menu_draw();

  if (click(B_OK))  { sndOk();   switchTo((AppId)MENU[menuSel].app); }
  if (longP(B_OK))  { sndBack(); switchTo(APP_HOME); }
}

/* ------------------------ NOTAS --------------------------- */
const char nt0[] PROGMEM = "Lock in";
const char nt1[] PROGMEM = "5 min mas";
const char nt2[] PROGMEM = "Una cosa a la vez";
const char nt3[] PROGMEM = "Respira";
const char nt4[] PROGMEM = "Bebe agua";
const char nt5[] PROGMEM = "Estirate";
const char nt6[] PROGMEM = "Postura!";
const char nt7[] PROGMEM = "Deja el movil";
const char nt8[] PROGMEM = "Casi esta";
const char nt9[] PROGMEM = "Vas bien";
const char* const NOTES[] PROGMEM = {
  nt0, nt1, nt2, nt3, nt4, nt5, nt6, nt7, nt8, nt9
};
const uint8_t NOTES_N = sizeof(NOTES) / sizeof(NOTES[0]);

uint8_t  noteIdx = 0;
uint32_t noteT = 0;
bool     noteAuto = true;

void note_draw() {
  char buf[18];
  strncpy_P(buf, (PGM_P)pgm_read_word(&NOTES[noteIdx]), 17);
  buf[17] = '\0';
  printCentered(0, buf);
  clearRow(1);
  lcd.setCursor(0, 1);
  lcd.print(noteAuto ? F("auto ") : F("     "));
  lcd.setCursor(12, 1);
  lcd.print(noteIdx + 1); lcd.print('/'); lcd.print(NOTES_N);
}
void notes_enter() { lcd.clear(); noteT = millis(); note_draw(); }
void notes_loop() {
  bool ch = false;
  if (press(B_RIGHT) || press(B_DOWN)) { noteIdx = (noteIdx + 1) % NOTES_N; ch = true; }
  if (press(B_LEFT)  || press(B_UP))   { noteIdx = (noteIdx + NOTES_N - 1) % NOTES_N; ch = true; }
  if (click(B_OK)) { noteAuto = !noteAuto; ch = true; sndOk(); }

  if (noteAuto && millis() - noteT > 4000) { noteIdx = (noteIdx + 1) % NOTES_N; ch = true; }

  if (ch) { noteT = millis(); note_draw(); }
  if (longP(B_OK)) { sndBack(); switchTo(APP_MENU); }
}

/* ---------------------- AJUSTES --------------------------- */
enum { ST_HORA = 0, ST_SONIDO, ST_LUZ, ST_BRILLO, ST_CALIB, ST_N };
uint8_t setSel = 0;
bool    setDirty = true;
bool    setEditTime = false;
uint8_t setTimeField = 0;              // 0 = horas, 1 = minutos
const uint16_t BL_OPTS[] = { 0, 15, 30, 60, 120 };

void set_drawValue() {
  clearRow(1);
  lcd.setCursor(0, 1);
  switch (setSel) {
    case ST_HORA:
      print2(clkH()); lcd.print(':'); print2(clkM());
      if (setEditTime) {
        lcd.print(F("  <edit>"));
        lcd.setCursor(setTimeField ? 3 : 0, 1);
        lcd.blink();
      } else { lcd.noBlink(); lcd.print(F("  OK=ajustar")); }
      break;
    case ST_SONIDO: lcd.print(cfg.mute ? F("Silencio") : F("Activado")); break;
    case ST_LUZ:
      if (cfg.blTimeout == 0) lcd.print(F("Siempre ON"));
      else { lcd.print(cfg.blTimeout); lcd.print(F(" s")); }
      break;
    case ST_BRILLO: lcd.print((int)cfg.brightness); break;
    case ST_CALIB:  lcd.print(cfg.calib); lcd.print(F(" ms/h")); break;
  }
}
void set_draw() {
  clearRow(0);
  lcd.setCursor(0, 0);
  switch (setSel) {
    case ST_HORA:   lcd.print(F("Hora"));            break;
    case ST_SONIDO: lcd.print(F("Sonido"));          break;
    case ST_LUZ:    lcd.print(F("Apagar luz"));      break;
    case ST_BRILLO: lcd.print(F("Brillo"));          break;
    case ST_CALIB:  lcd.print(F("Calibrar reloj"));  break;
  }
  set_drawValue();
  setDirty = false;
}
void settings_enter() {
  lcd.clear(); setEditTime = false; setDirty = true;
}
void settings_loop() {
  if (setEditTime) {                      // --- editor de hora ---
    int32_t delta = 0;
    if (rep(B_UP))   delta = +1;
    if (rep(B_DOWN)) delta = -1;
    if (delta != 0) {
      int32_t step = setTimeField ? 60 : 3600;
      clkSec = (clkSec + 86400UL + delta * step) % 86400UL;
      if (!setTimeField) { } else clkSec -= clkSec % 60;   // seg a 0
      clkAcc = 0;
      sndMove(); setDirty = true;
    }
    if (press(B_LEFT) || press(B_RIGHT)) { setTimeField ^= 1; setDirty = true; }
    if (click(B_OK)) { setEditTime = false; lcd.noBlink(); sndOk(); setDirty = true; }
    if (longP(B_OK)) { setEditTime = false; lcd.noBlink(); sndBack(); setDirty = true; }
    if (setDirty) set_draw();
    return;
  }

  if (rep(B_UP))   { setSel = (setSel + ST_N - 1) % ST_N; sndMove(); setDirty = true; }
  if (rep(B_DOWN)) { setSel = (setSel + 1) % ST_N;        sndMove(); setDirty = true; }

  int8_t d = 0;
  if (rep(B_RIGHT)) d = +1;
  if (rep(B_LEFT))  d = -1;
  if (d != 0) {
    switch (setSel) {
      case ST_SONIDO: cfg.mute = !cfg.mute; break;
      case ST_LUZ: {
        uint8_t i = 0;
        for (uint8_t k = 0; k < 5; k++) if (BL_OPTS[k] == cfg.blTimeout) i = k;
        i = (i + 5 + d) % 5;
        cfg.blTimeout = BL_OPTS[i];
        break;
      }
      case ST_BRILLO: {
        int16_t v = cfg.brightness + d * 32;
        if (v < 32) v = 32;
        if (v > 255) v = 255;
        cfg.brightness = v; backlightApply(cfg.brightness);
        break;
      }
      case ST_CALIB: {
        int32_t v = cfg.calib + d * 10;
        if (v < -2000) v = -2000;
        if (v > 2000) v = 2000;
        cfg.calib = v;
        break;
      }
    }
    sndMove(); setDirty = true;
  }

  if (click(B_OK) && setSel == ST_HORA) {
    setEditTime = true; setTimeField = 0; sndOk(); setDirty = true;
  }
  if (setDirty) set_draw();

  if (longP(B_OK)) { cfgSave(); sndBack(); switchTo(APP_MENU); }
}

/* ====================== REFLEJOS ==========================
   5 rondas. Espera aleatoria, aparece un boton en pantalla y se
   mide lo que tardas en pulsarlo. Adelantarse o fallar de boton
   penaliza. Guarda el mejor tiempo en EEPROM.
   =========================================================== */
enum { RF_IDLE = 0, RF_WAIT, RF_GO, RF_SHOW, RF_END };
#define RF_ROUNDS   5
#define RF_TIMEOUT  3000
#define DIRMASK ((1<<B_UP)|(1<<B_DOWN)|(1<<B_LEFT)|(1<<B_RIGHT))

const char rfn0[] PROGMEM = "ARRIBA";
const char rfn1[] PROGMEM = "ABAJO";
const char rfn2[] PROGMEM = "IZQUIERDA";
const char rfn3[] PROGMEM = "DERECHA";
const char* const RF_NAMES[] PROGMEM = { rfn0, rfn1, rfn2, rfn3 };
const uint8_t RF_BTN[4] = { B_UP, B_DOWN, B_LEFT, B_RIGHT };

uint8_t  rfState, rfRound, rfTarget;
uint32_t rfT, rfSum;
uint16_t rfBestRun, rfLastMs;

void rf_drawIdle() {
  lcd.clear();
  printAt(0, 0, F("REFLEJOS"));
  lcd.setCursor(0, 1);
  if (cfg.reflexBest) { lcd.print(F("Rec ")); lcd.print(cfg.reflexBest);
                        lcd.print(F("ms OK=ir")); }
  else                  lcd.print(F("OK = empezar"));
}
void rf_nextRound() {
  rfState = RF_WAIT;
  rfT = millis() + random(1200, 4000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Preparado...")); 
  lcd.setCursor(0, 1); lcd.print(rfRound + 1); lcd.print('/');
  lcd.print(RF_ROUNDS);
  rfTarget = random(4);
}
void rf_score(uint16_t ms, bool ok) {
  char buf[17];
  rfLastMs = ms; rfSum += ms;
  if (ok && ms < rfBestRun) rfBestRun = ms;
  rfState = RF_SHOW; rfT = millis();
  lcd.clear();
  lcd.setCursor(0, 0);
  if (ok) { lcd.print(ms); lcd.print(F(" ms")); sndOk(); }
  else    { lcd.print(F("FALLO"));              sndError(); }
  strncpy_P(buf, (PGM_P)pgm_read_word(&RF_NAMES[rfTarget]), 16);
  buf[16] = '\0';
  lcd.setCursor(0, 1); lcd.print(buf);
}
void rf_end() {
  rfState = RF_END;
  uint16_t avg = rfSum / RF_ROUNDS;
  bool record = (rfBestRun < 9999) &&
                (cfg.reflexBest == 0 || rfBestRun < cfg.reflexBest);
  if (record) { cfg.reflexBest = rfBestRun; cfgSave(); }
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(F("Media ")); lcd.print(avg);
  lcd.print(F(" ms"));
  lcd.setCursor(0, 1);
  if (record) { lcd.print(F("RECORD! ")); lcd.print(rfBestRun);
                beep(4, 60, 70); }
  else        { lcd.print(F("Mejor  ")); lcd.print(rfBestRun); }
}

void reflex_enter() {
  randomSeed(micros());           // el instante de tu pulsacion
  rfState = RF_IDLE;              // es una semilla estupenda
  rf_drawIdle();
}
void reflex_loop() {
  switch (rfState) {
    case RF_IDLE:
      if (click(B_OK)) {
        rfRound = 0; rfSum = 0; rfBestRun = 9999;
        randomSeed(micros());
        rf_nextRound();
      }
      break;

    case RF_WAIT:
      if (bPressed & DIRMASK) {            // te has adelantado
        rf_score(RF_TIMEOUT, false);
        lcd.setCursor(0, 1); lcd.print(F("Te adelantaste "));
      } else if ((int32_t)(millis() - rfT) >= 0) {
        rfState = RF_GO; rfT = millis();
        char buf[17];
        strncpy_P(buf, (PGM_P)pgm_read_word(&RF_NAMES[rfTarget]), 16);
        buf[16] = '\0';
        lcd.clear();
        printCentered(0, buf);
        beep(1, 30, 0);
      }
      break;

    case RF_GO:
      if (press(RF_BTN[rfTarget])) {
        rf_score((uint16_t)(millis() - rfT), true);
      } else if (bPressed & DIRMASK) {
        rf_score(RF_TIMEOUT, false);
      } else if (millis() - rfT > RF_TIMEOUT) {
        rf_score(RF_TIMEOUT, false);
      }
      break;

    case RF_SHOW:
      if (millis() - rfT > 1200) {
        rfRound++;
        if (rfRound >= RF_ROUNDS) rf_end(); else rf_nextRound();
      }
      break;

    case RF_END:
      if (click(B_OK)) { rfState = RF_IDLE; rf_drawIdle(); }
      break;
  }
  if (longP(B_OK)) { sndBack(); switchTo(APP_MENU); }
}

/* ====================== POMODORO ==========================
   Cuenta atras en digitos grandes. OK = start / pausa.
   En reposo UP/DOWN cambian los minutos de trabajo y LEFT/RIGHT
   los de descanso. En marcha, RIGHT salta de fase.
   =========================================================== */
enum { PM_IDLE = 0, PM_RUN, PM_PAUSE, PM_MSG };

uint8_t  pmState, pmCycle = 1, pmDone = 0;
bool     pmIsWork = true;
uint32_t pmRemain, pmLastTick, pmT;
uint8_t  pmLastMin = 255, pmLastSec = 255;
const __FlashStringHelper *pmMsg;

void pm_drawIdle() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Pomo ")); lcd.print(cfg.pomoWork); lcd.print('/');
  lcd.print(cfg.pomoBreak); lcd.print(F(" x")); lcd.print(cfg.pomoCycles);
  lcd.setCursor(0, 1);
  lcd.print(F("OK=ir  hoy:")); lcd.print(pmDone);
}
void pm_showMsg(const __FlashStringHelper *m) {
  pmMsg = m; pmState = PM_MSG; pmT = millis();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(m);
  lcd.setCursor(0, 1);
  lcd.print(pmIsWork ? F("Trabajo ") : F("Descanso "));
  lcd.print(pmIsWork ? cfg.pomoWork : cfg.pomoBreak);
  lcd.print(F(" min"));
}
void pm_startPhase(bool work) {
  pmIsWork = work;
  pmRemain = (uint32_t)(work ? cfg.pomoWork : cfg.pomoBreak) * 60000UL;
  pmLastTick = millis();
  pmLastMin = pmLastSec = 255;
  // Nota contextual: el mensaje adecuado en el momento adecuado
  if (work) pm_showMsg(F("Lock in"));
  else      pm_showMsg(F("Estirate"));
}
void pm_drawTimer() {
  uint32_t s = (pmRemain + 999) / 1000;      // redondeo hacia arriba
  uint8_t mm = s / 60, ss = s % 60;
  if (mm != pmLastMin) {
    pmLastMin = mm;
    bigDigit(mm / 10, 0); bigDigit(mm % 10, 3);
  }
  if (ss != pmLastSec) {
    pmLastSec = ss;
    bigDigit(ss / 10, 7); bigDigit(ss % 10, 10);
    lcd.setCursor(6, 0); lcd.print('.');
    lcd.setCursor(6, 1); lcd.print('.');
    lcd.setCursor(13, 0);
    if (pmState == PM_PAUSE)  lcd.print(F("PAU"));
    else lcd.print(pmIsWork ? F("TRA") : F("DES"));
    lcd.setCursor(13, 1);
    lcd.print(pmCycle); lcd.print('/'); lcd.print(cfg.pomoCycles);
  }
}
void pm_phaseEnd() {
  if (pmIsWork) {
    pmDone++;
    beep(3, 250, 150);                       // fin de trabajo: largo
    if (pmCycle >= cfg.pomoCycles) {
      pmCycle = 1;
      pm_startPhase(false);
      pm_showMsg(F("Ciclo completo!"));
    } else {
      pmCycle++;
      pm_startPhase(false);
    }
  } else {
    beep(2, 80, 90);                         // fin de descanso: corto
    pm_startPhase(true);
  }
}

void pomo_enter() {
  loadGlyphs();
  pmState = PM_IDLE;
  pm_drawIdle();
}
void pomo_loop() {
  switch (pmState) {
    case PM_IDLE: {
      bool ch = false;
      if (rep(B_UP)    && cfg.pomoWork  < 90) { cfg.pomoWork++;  ch = true; }
      if (rep(B_DOWN)  && cfg.pomoWork  >  1) { cfg.pomoWork--;  ch = true; }
      if (rep(B_RIGHT) && cfg.pomoBreak < 60) { cfg.pomoBreak++; ch = true; }
      if (rep(B_LEFT)  && cfg.pomoBreak >  1) { cfg.pomoBreak--; ch = true; }
      if (ch) { sndMove(); pm_drawIdle(); }
      if (click(B_OK)) {
        cfgSave(); pmCycle = 1;
        lcd.clear(); pm_startPhase(true);
      }
      break;
    }

    case PM_MSG:
      if (millis() - pmT > 2200) {
        pmState = PM_RUN;
        lcd.clear();
        pmLastMin = pmLastSec = 255;
        pmLastTick = millis();
      }
      if (click(B_OK)) {                     // saltar el mensaje
        pmState = PM_RUN; lcd.clear();
        pmLastMin = pmLastSec = 255; pmLastTick = millis();
      }
      break;

    case PM_RUN: {
      uint32_t now = millis();
      uint32_t dt = now - pmLastTick;
      pmLastTick = now;
      if (pmRemain > dt) pmRemain -= dt; else { pm_phaseEnd(); break; }
      pm_drawTimer();
      if (click(B_OK))   { pmState = PM_PAUSE; pmLastSec = 255;
                           sndMove(); pm_drawTimer(); }
      if (press(B_RIGHT)) { sndMove(); pm_phaseEnd(); }
      if (press(B_LEFT))  { sndMove(); pm_startPhase(pmIsWork); }
      break;
    }

    case PM_PAUSE:
      if (click(B_OK)) {
        pmState = PM_RUN; pmLastTick = millis();
        pmLastSec = 255; sndOk();
      }
      break;
  }
  if (longP(B_OK)) { cfgSave(); sndBack(); switchTo(APP_MENU); }
}

/* ======================== SNAKE ===========================
   El truco para tener mas resolucion: cada celda del LCD se
   parte en mitad superior e inferior, asi que el tablero real
   es de 16 x 4 = 64 casillas. Se recarga la CGRAM con 7
   caracteres propios al entrar (la fuente grande se restaura
   al volver al reloj o al pomodoro).
   =========================================================== */
#define SNK_W   16
#define SNK_H    4
#define SNK_MAX 48                 // longitud maxima de la serpiente

// 0 arriba lleno, 1 abajo lleno, 2 ambos, 3 punto arriba,
// 4 punto abajo, 5 bloque arriba + punto abajo, 6 al reves
const uint8_t SNK_GLYPHS[7][8] PROGMEM = {
  {B11111,B11111,B11111,B00000,B00000,B00000,B00000,B00000},
  {B00000,B00000,B00000,B00000,B11111,B11111,B11111,B00000},
  {B11111,B11111,B11111,B00000,B11111,B11111,B11111,B00000},
  {B00000,B01110,B01110,B00000,B00000,B00000,B00000,B00000},
  {B00000,B00000,B00000,B00000,B00000,B01110,B01110,B00000},
  {B11111,B11111,B11111,B00000,B00000,B01110,B01110,B00000},
  {B00000,B01110,B01110,B00000,B11111,B11111,B11111,B00000}
};
// indice = arriba*3 + abajo, con 0=vacio 1=serpiente 2=comida
const uint8_t SNK_MAP[9] = { 32, 1, 4, 0, 2, 5, 3, 6, 3 };

uint8_t  snkGrid[SNK_W * SNK_H];
uint8_t  snkBody[SNK_MAX];
uint8_t  snkHead, snkLen, snkScore;
int8_t   snkDX, snkDY, snkNX, snkNY;
uint16_t snkInterval;
uint32_t snkTick;
bool     snkAlive, snkPaused;

void snk_loadGlyphs() {
  uint8_t tmp[8];
  for (uint8_t i = 0; i < 7; i++) {
    memcpy_P(tmp, SNK_GLYPHS[i], 8);
    lcd.createChar(i, tmp);
  }
}
void snk_food() {
  uint8_t libres = 0;
  for (uint8_t i = 0; i < SNK_W * SNK_H; i++) if (!snkGrid[i]) libres++;
  if (!libres) return;
  uint8_t n = random(libres);
  for (uint8_t i = 0; i < SNK_W * SNK_H; i++) {
    if (!snkGrid[i]) { if (!n) { snkGrid[i] = 2; return; } n--; }
  }
}
void snk_render() {
  for (uint8_t r = 0; r < 2; r++) {
    lcd.setCursor(0, r);
    for (uint8_t x = 0; x < SNK_W; x++) {
      uint8_t t = snkGrid[(2 * r)     * SNK_W + x];
      uint8_t b = snkGrid[(2 * r + 1) * SNK_W + x];
      wr(SNK_MAP[t * 3 + b]);
    }
  }
}
void snk_over() {
  snkAlive = false;
  bool record = (snkScore > cfg.snakeHi);
  if (record) { cfg.snakeHi = snkScore; cfgSave(); }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Fin! Puntos ")); lcd.print(snkScore);
  lcd.setCursor(0, 1);
  if (record) { lcd.print(F("RECORD! OK=otra")); beep(4, 60, 70); }
  else { lcd.print(F("Rec ")); lcd.print(cfg.snakeHi);
         lcd.print(F("  OK=otra")); sndError(); }
}
void snk_reset() {
  memset(snkGrid, 0, sizeof(snkGrid));
  snkLen = 3; snkHead = 2; snkScore = 0;
  for (uint8_t i = 0; i < 3; i++) {
    snkBody[i] = 1 * SNK_W + (2 + i);
    snkGrid[snkBody[i]] = 1;
  }
  snkDX = snkNX = 1; snkDY = snkNY = 0;
  snkInterval = 400; snkTick = millis();
  snkAlive = true; snkPaused = false;
  snk_food();
  lcd.clear();
  snk_render();
}
void snk_step() {
  snkDX = snkNX; snkDY = snkNY;
  int8_t hx = snkBody[snkHead] % SNK_W + snkDX;
  int8_t hy = snkBody[snkHead] / SNK_W + snkDY;
  if (hx < 0) hx = SNK_W - 1; else if (hx >= SNK_W) hx = 0;   // el
  if (hy < 0) hy = SNK_H - 1; else if (hy >= SNK_H) hy = 0;   // tablero
  uint8_t np = hy * SNK_W + hx;                               // da la
                                                              // vuelta
  uint8_t tailPos = snkBody[(snkHead + SNK_MAX - snkLen + 1) % SNK_MAX];
  bool ate = (snkGrid[np] == 2);

  // chocar con la cola no cuenta: en este mismo turno se aparta
  if (snkGrid[np] == 1 && np != tailPos) { snk_over(); return; }

  snkHead = (snkHead + 1) % SNK_MAX;
  snkBody[snkHead] = np;
  if (!ate) snkGrid[tailPos] = 0;
  else {
    if (snkLen < SNK_MAX) snkLen++;
    snkScore++;
    beep(1, 25, 0);
    snk_food();
    if (snkInterval > 150) snkInterval -= 12;
  }
  snkGrid[np] = 1;
  snk_render();
}

void snake_enter() {
  snk_loadGlyphs();
  randomSeed(micros());
  snk_reset();
}
void snake_loop() {
  if (snkAlive) {
    // el giro de 180 grados se ignora
    if (press(B_UP)    && snkDY == 0) { snkNX = 0; snkNY = -1; }
    if (press(B_DOWN)  && snkDY == 0) { snkNX = 0; snkNY = +1; }
    if (press(B_LEFT)  && snkDX == 0) { snkNX = -1; snkNY = 0; }
    if (press(B_RIGHT) && snkDX == 0) { snkNX = +1; snkNY = 0; }

    if (click(B_OK)) {
      snkPaused = !snkPaused;
      sndMove();
      if (snkPaused) { lcd.setCursor(0, 0); lcd.print(F("  PAUSA  ")); }
      else { snkTick = millis(); snk_render(); }
    }
    if (!snkPaused && millis() - snkTick >= snkInterval) {
      snkTick = millis();
      snk_step();
    }
  } else {
    if (click(B_OK)) snk_reset();
  }
  if (longP(B_OK)) { sndBack(); switchTo(APP_MENU); }
}

/* ========================= DINO ===========================
   Corredor infinito sobre la misma rejilla de 16x4 que Snake, y
   sobre el mismo array: los dos juegos nunca corren a la vez, asi
   que reutilizar snkGrid ahorra 64 bytes de RAM. Los cactus se
   dibujan como bloques y el dino como punto, que es lo que los
   distingue de un vistazo.
   =========================================================== */
#define DN_W 16

const int8_t DN_ARC[] = { 2, 1, 0, 0, 1, 2 };   // filas durante el salto
#define DN_AIR (int8_t)(sizeof(DN_ARC))

uint8_t  dnObst[DN_W];      // 0 nada, 1 cactus bajo, 2 cactus alto
uint8_t  dnDinoY, dnGap;
int8_t   dnJump;            // fase del salto, -1 si esta en el suelo
uint16_t dnScore, dnInterval;
uint32_t dnTick;
bool     dnAlive, dnPaused;

void dn_render() {
  memset(snkGrid, 0, SNK_W * SNK_H);
  for (uint8_t x = 0; x < DN_W; x++) {
    if (dnObst[x] >= 1) snkGrid[3 * SNK_W + x] = 1;
    if (dnObst[x] == 2) snkGrid[2 * SNK_W + x] = 1;
  }
  snkGrid[dnDinoY * SNK_W + 2] = 2;
  snk_render();
}
void dn_over() {
  dnAlive = false;
  bool record = (dnScore > cfg.dinoHi);
  if (record) { cfg.dinoHi = dnScore; cfgSave(); }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Fin! Puntos ")); lcd.print(dnScore);
  lcd.setCursor(0, 1);
  if (record) { lcd.print(F("RECORD! OK=otra")); beep(4, 60, 70); }
  else { lcd.print(F("Rec ")); lcd.print(cfg.dinoHi);
         lcd.print(F("  OK=otra")); sndError(); }
}
void dn_reset() {
  memset(dnObst, 0, sizeof(dnObst));
  dnDinoY = 3; dnJump = -1; dnGap = 8;
  dnScore = 0; dnInterval = 220;
  dnTick = millis(); dnAlive = true; dnPaused = false;
  lcd.clear();
  dn_render();
}
void dn_step() {
  for (uint8_t x = 0; x < DN_W - 1; x++) dnObst[x] = dnObst[x + 1];
  dnObst[DN_W - 1] = 0;

  if (dnGap) dnGap--;
  else if (random(100) < 28) {
    dnObst[DN_W - 1] = (random(100) < 30) ? 2 : 1;   // alto o bajo
    dnGap = 4 + random(4);                            // separacion minima
  }

  if (dnJump >= 0) {
    dnDinoY = DN_ARC[dnJump];
    dnJump++;
    if (dnJump >= DN_AIR) { dnJump = -1; dnDinoY = 3; }
  }

  uint8_t o = dnObst[2];
  if ((o == 1 && dnDinoY == 3) || (o == 2 && dnDinoY >= 2)) { dn_over(); return; }

  dnScore++;
  if (dnScore % 40 == 0 && dnInterval > 90) dnInterval -= 8;
  dn_render();
}

void dino_enter() {
  snk_loadGlyphs();
  randomSeed(micros());
  dn_reset();
}
void dino_loop() {
  if (dnAlive) {
    if (press(B_UP) && dnJump < 0) { dnJump = 0; beep(1, 20, 0); }
    if (click(B_OK)) {
      dnPaused = !dnPaused;
      sndMove();
      if (dnPaused) { lcd.setCursor(0, 0); lcd.print(F("  PAUSA  ")); }
      else { dnTick = millis(); dn_render(); }
    }
    if (!dnPaused && millis() - dnTick >= dnInterval) {
      dnTick = millis();
      dn_step();
    }
  } else {
    if (click(B_OK)) dn_reset();
  }
  if (longP(B_OK)) { sndBack(); switchTo(APP_MENU); }
}

/* ========================= SIMON ==========================
   Con un zumbador ACTIVO no hay notas, solo encendido y apagado,
   asi que cada boton se identifica por su RITMO: uno corto, dos
   cortos, uno largo, tres muy cortos. Si algun dia pones un
   zumbador pasivo, cambia PASSIVE_BUZZER a 1 arriba del archivo
   y pasan a ser cuatro notas, como el Simon original.
   =========================================================== */
#define SM_MAX 32
enum { SM_IDLE = 0, SM_SHOW, SM_GAP, SM_WAIT, SM_NEXT, SM_OVER };

uint8_t  smSeq[SM_MAX];
uint8_t  smLen, smPos, smShow, smState;
uint32_t smT;
uint16_t smDur;

#if PASSIVE_BUZZER
const uint16_t SM_FREQ[4] = { 330, 262, 392, 494 };
#else
const uint8_t  SM_N[4]   = {   1,   2,   1,   3 };
const uint16_t SM_ON[4]  = { 130,  60, 320,  45 };
const uint16_t SM_OFF[4] = {   0,  90,   0,  60 };
#endif

// Emite la senal del boton b y devuelve lo que dura, para que la
// maquina de estados sepa cuando seguir sin bloquear.
uint16_t sm_signal(uint8_t b) {
#if PASSIVE_BUZZER
  if (!isMuted()) tone(PIN_BUZZER, SM_FREQ[b], 280);
  return 300;
#else
  beep(SM_N[b], SM_ON[b], SM_OFF[b]);
  return SM_N[b] * (SM_ON[b] + SM_OFF[b]) + 130;
#endif
}
void sm_name(uint8_t b) {
  char buf[17];
  strncpy_P(buf, (PGM_P)pgm_read_word(&RF_NAMES[b]), 16);
  buf[16] = '\0';
  printCentered(0, buf);
}
void sm_drawIdle() {
  lcd.clear();
  printAt(0, 0, F("SIMON"));
  lcd.setCursor(0, 1);
  if (cfg.simonHi) { lcd.print(F("Rec ")); lcd.print(cfg.simonHi);
                     lcd.print(F("  OK=ir")); }
  else               lcd.print(F("OK = empezar"));
}
void sm_over() {
  smState = SM_OVER;
  uint8_t score = smLen ? smLen - 1 : 0;
  bool record = (score > cfg.simonHi);
  if (record) { cfg.simonHi = score; cfgSave(); }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Fallo! Ronda ")); lcd.print(score);
  lcd.setCursor(0, 1);
  if (record) { lcd.print(F("RECORD! OK=otra")); beep(4, 60, 70); }
  else { lcd.print(F("Rec ")); lcd.print(cfg.simonHi);
         lcd.print(F("  OK=otra")); sndError(); }
}
void sm_grow() {
  if (smLen < SM_MAX) smSeq[smLen++] = random(4);
  smShow = 0; smPos = 0;
  smState = SM_NEXT; smT = millis();
  lcd.clear();
  lcd.setCursor(0, 1); lcd.print(F("Ronda ")); lcd.print(smLen);
}

void simon_enter() {
  randomSeed(micros());
  smState = SM_IDLE; smLen = 0;
  sm_drawIdle();
}
void simon_loop() {
  switch (smState) {
    case SM_IDLE:
      if (click(B_OK)) { smLen = 0; randomSeed(micros()); sm_grow(); }
      break;

    case SM_NEXT:                       // pausa antes de reproducir
      if (millis() - smT > 700) smState = SM_SHOW;
      break;

    case SM_SHOW:                       // emite un elemento
      sm_name(smSeq[smShow]);
      smDur = sm_signal(smSeq[smShow]);
      smT = millis();
      smState = SM_GAP;
      break;

    case SM_GAP:                        // espera a que termine
      if (millis() - smT > smDur) {
        clearRow(0);
        smShow++;
        if (smShow >= smLen) {
          smState = SM_WAIT; smT = millis();
          printCentered(0, "Tu turno");
        } else smState = SM_SHOW;
      }
      break;

    case SM_WAIT: {
      uint8_t got = 255;
      for (uint8_t i = 0; i < 4; i++) if (press(RF_BTN[i])) got = i;
      if (got != 255) {
        smT = millis();
        if (got == smSeq[smPos]) {
          sm_name(got);
          sm_signal(got);
          smPos++;
          if (smPos >= smLen) sm_grow();
        } else sm_over();
      } else if (millis() - smT > 5000) sm_over();   // se acabo el tiempo
      break;
    }

    case SM_OVER:
      if (click(B_OK)) { smState = SM_IDLE; sm_drawIdle(); }
      break;
  }
  if (longP(B_OK)) { sndBack(); switchTo(APP_MENU); }
}

/* ======================= SISTEMA ==========================
   Voltaje real de alimentacion, temperatura del chip, RAM libre
   y tiempo encendido. Sin un solo componente adicional: todo
   sale de perifericos internos del ATmega328P.
   =========================================================== */

// RAM libre = hueco entre el final del monton y la pila
int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

// Mide AVcc comparandolo con la referencia interna de 1,1 V.
// El truco: en vez de medir una tension con Vcc como referencia,
// se mide la referencia usando Vcc como escala y se despeja.
uint16_t readVccMv() {
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delayMicroseconds(2000);          // asentar la referencia
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC));
  uint16_t adc = ADC;
  if (!adc) return 0;
  return (uint16_t)(1125300L / adc);            // 1,1 * 1023 * 1000
}

// Sensor de temperatura interno, canal 8 del ADC. El datasheet da
// unos 1 mV/C y ~324 cuentas a 25 C, pero SIN CALIBRAR el error
// es de +-10 C: de ahi el offset ajustable.
int16_t readTempC() {
  ADMUX = _BV(REFS1) | _BV(REFS0) | _BV(MUX3);
  delayMicroseconds(2000);
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC));
  int16_t adc = ADC;
  return adc - 324 + 25 + cfg.tempOffset;
}

uint32_t stT = 0;

void stats_draw() {
  uint16_t mv = readVccMv();
  int16_t  tc = readTempC();
  int      fr = freeRam();
  uint32_t up = millis() / 1000UL;

  clearRow(0);
  lcd.setCursor(0, 0);
  lcd.print(mv / 1000); lcd.print('.');
  uint8_t cent = (mv % 1000) / 10;
  if (cent < 10) lcd.print('0');
  lcd.print(cent); lcd.print('V');
  lcd.setCursor(9, 0);
  lcd.print(tc); lcd.write(223); lcd.print('C');   // 223 = simbolo de grado

  clearRow(1);
  lcd.setCursor(0, 1);
  lcd.print(F("RAM ")); lcd.print(fr);
  lcd.setCursor(9, 1);
  lcd.print(up / 3600UL); lcd.print('h');
  uint8_t mi = (up / 60UL) % 60UL;
  if (mi < 10) lcd.print('0');
  lcd.print(mi); lcd.print('m');
}

void stats_enter() { lcd.clear(); stats_draw(); stT = millis(); }
void stats_loop() {
  if (millis() - stT > 1000) { stT = millis(); stats_draw(); }
  // calibracion del sensor de temperatura
  if (rep(B_UP)   && cfg.tempOffset <  40) { cfg.tempOffset++; sndMove(); stats_draw(); }
  if (rep(B_DOWN) && cfg.tempOffset > -40) { cfg.tempOffset--; sndMove(); stats_draw(); }
  if (longP(B_OK)) { cfgSave(); sndBack(); switchTo(APP_MENU); }
}

/* ================ TABLA DE APLICACIONES =================== */
struct App { void (*enter)(); void (*loop)(); };
const App APPS[APP_COUNT] = {
  { home_enter,     home_loop     },   // APP_HOME
  { menu_enter,     menu_loop     },   // APP_MENU
  { snake_enter,    snake_loop    },   // APP_SNAKE
  { dino_enter,     dino_loop     },   // APP_DINO
  { simon_enter,    simon_loop    },   // APP_SIMON
  { reflex_enter,   reflex_loop   },   // APP_REFLEX
  { pomo_enter,     pomo_loop     },   // APP_POMO
  { notes_enter,    notes_loop    },   // APP_NOTES
  { stats_enter,    stats_loop    },   // APP_STATS
  { settings_enter, settings_loop }    // APP_SETTINGS
};

void switchTo(AppId id) {
  appPrev = appCur;
  appCur = id;
  appEnterMs = millis();
  bIgnore = true;              // no arrastrar pulsaciones a la app nueva
  lcd.noBlink();
  APPS[appCur].enter();
}

/* ======================== SETUP =========================== */
void setup() {
  for (uint8_t i = 0; i < B_COUNT; i++) pinMode(BTN_PIN[i], INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#if USE_BACKLIGHT_PIN
  pinMode(PIN_BACKLIGHT, OUTPUT);
#endif

  cfgLoad();
  backlightApply(cfg.brightness);
  blLast = millis();

  lcd.begin(16, 2);
  loadGlyphs();

  lcd.clear();
  printAt(2, 0, F("CONSOLA 1602"));
  printAt(6, 1, F("v1.0"));
  beep(1, 80, 0);
  uint32_t t0 = millis();       // espera sin bloquear el buzzer:
  while (millis() - t0 < 900) { // con delay() se quedaba pitando
    beepUpdate();               // los 900 ms enteros
  }

  clkLastMs = millis();
  switchTo(APP_HOME);
}

/* ========================= LOOP ===========================
   Regla de oro: nada bloquea. Ni delay() ni while() de espera.
   =========================================================== */
void loop() {
  buttonsUpdate();
  clockUpdate();
  beepUpdate();
  backlightUpdate();

  APPS[appCur].loop();

  // Vuelta automatica al reloj tras 30 s sin tocar nada
  // (solo desde el menu y las notas, no desde juegos ni ajustes)
  if ((appCur == APP_MENU || appCur == APP_NOTES) &&
      millis() - blLast > 30000UL) {
    switchTo(APP_HOME);
  }
}
