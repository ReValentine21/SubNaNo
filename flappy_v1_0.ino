/*
 * ╔══════════════════════════════════════╗
 * ║   SUBNO-FLAP  v1.1  by SubNoNo      ║
 * ║   Arduino Nano | SSD1306 128x64     ║
 * ║   D2 = Button  |  D1 = Buzzer(inv) ║
 * ╚══════════════════════════════════════╝
 *
 *  Управление (1 кнопка):
 *    МЕНЮ    → нажать = старт
 *    ИГРА    → нажать = прыжок
 *    КОНЕЦ   → 1 клик = меню, 2 клика = рестарт
 *
 *  Баззер: активный, подключён через инвертор (D1 HIGH = тишина)
 *  Звуки: прыжок, очко, смерть, рекорд, приветствие
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ── ПИНЫ ──────────────────────────────────────────────────────────────
#define BTN   2          // кнопка (INPUT_PULLUP → LOW = нажато)
#define BUZ   3          // баззер инвертированный (LOW = пищит)
#define W     128
#define H     64

Adafruit_SSD1306 d(W, H, &Wire, -1);

// ── ЗВУК (неблокирующий) ───────────────────────────────────────────────
struct Beep { unsigned int freq; unsigned int dur; };

// Очередь нот
#define QUEUE_SZ 16
Beep bQueue[QUEUE_SZ];
byte bHead = 0, bTail = 0;
unsigned long bEnd = 0;
bool bActive = false;

// Добавить ноту в очередь
void beepQ(unsigned int freq, unsigned int dur){
  byte next = (bTail + 1) % QUEUE_SZ;
  if(next == bHead) return; // переполнение → игнор
  bQueue[bTail] = {freq, dur};
  bTail = next;
}

// Обновление баззера (вызывать каждый кадр)
void buzUpdate(){
  unsigned long now = millis();
  if(bActive){
    if(now >= bEnd){
      // Инвертированный баззер: HIGH = выкл
      tone(BUZ, 1); delay(0); noTone(BUZ); // сброс через noTone
      // Реально выключаем: для активного инверт. — HIGH = тихо
      // используем digitalWrite после noTone
      digitalWrite(BUZ, HIGH);
      bActive = false;
    }
  }
  if(!bActive && bHead != bTail){
    Beep b = bQueue[bHead];
    bHead = (bHead + 1) % QUEUE_SZ;
    if(b.freq == 0){
      // пауза
      bEnd = now + b.dur;
      bActive = true;
      digitalWrite(BUZ, HIGH); // тихо
    } else {
      tone(BUZ, b.freq, b.dur);
      bEnd = now + b.dur + 5;
      bActive = true;
    }
  }
}

// Мелодии
void sndJump(){  beepQ(880, 40); beepQ(1046,30); }
void sndScore(){ beepQ(1046,50); beepQ(1318,60); }
void sndDie(){   beepQ(440,80);  beepQ(330,80);  beepQ(220,150); }
void sndRecord(){ beepQ(1046,60);beepQ(1318,60);beepQ(1568,80);beepQ(2093,100); }
void sndMenu(){  beepQ(523,60);  beepQ(659,60);  beepQ(784,80);  }
void sndClick(){ beepQ(1200,25); }

// ── СОСТОЯНИЯ ─────────────────────────────────────────────────────────
enum State { MENU, PLAY, OVER };
State state = MENU;

// ── СПРАЙТЫ ПТИЧКИ ────────────────────────────────────────────────────
// 8×8 пиксельная птичка в трёх позициях
const unsigned char bird_up[] PROGMEM = {
  0b00010000,
  0b00111000,
  0b01111100,
  0b11111110,
  0b01111110,
  0b00111100,
  0b00011000,
  0b00000000
};
const unsigned char bird_mid[] PROGMEM = {
  0b00000000,
  0b00111000,
  0b01111100,
  0b11111110,
  0b11111110,
  0b01111100,
  0b00111000,
  0b00000000
};
const unsigned char bird_down[] PROGMEM = {
  0b00000000,
  0b00010000,
  0b00111000,
  0b01111100,
  0b11111110,
  0b01111100,
  0b00111000,
  0b00010000
};

// Иконка сердца для "рекорд"
const unsigned char heart[] PROGMEM = {
  0b01100110,
  0b11111111,
  0b11111111,
  0b01111110,
  0b00111100,
  0b00011000,
  0b00000000,
  0b00000000
};

// Иконка звезды для очка
const unsigned char star[] PROGMEM = {
  0b00010000,
  0b01111100,
  0b00111000,
  0b11101110,
  0b11000011,
  0b01000010,
  0b00000000,
  0b00000000
};

// ── ФИЗИКА ────────────────────────────────────────────────────────────
int   birdY    = 120;   // × 8 (fixed point)
int   birdVel  = 0;
const int GRAVITY  = 3;  // × 0.1 за кадр
const int JUMP_VEL = -25;// × 0.1

// ── ТРУБЫ ─────────────────────────────────────────────────────────────
#define PIPE_W    10
#define GAP_HALF  14
#define BIRD_X    22

int pipeX  = W;
int gapY   = 32;

// ── ОЧКИ / РЕКОРД ─────────────────────────────────────────────────────
int  score      = 0;
int  best       = 0;
bool newRecord  = false;

// ── СКОРОСТЬ ──────────────────────────────────────────────────────────
int  pipeSpeed  = 2;
byte delayMs    = 25;  // мс на кадр

// ── КНОПКА ────────────────────────────────────────────────────────────
bool lastBtn    = HIGH;
bool btnClick   = false;

// ── ДВОЙНОЙ КЛИК (GAME OVER) ──────────────────────────────────────────
byte           dClicks  = 0;
unsigned long  dClickT  = 0;

// ── FPS ───────────────────────────────────────────────────────────────
unsigned long frameT = 0;

// ── ЭФФЕКТ СМЕРТИ ─────────────────────────────────────────────────────
byte deathAnim = 0;   // счётчик кадров анимации смерти

// ── АНИМАЦИЯ МЕНЮ ────────────────────────────────────────────────────
byte menuFrame  = 0;
int  menuBirdY  = 30;
int  menuBirdV  = 0;

// ── EEPROM ────────────────────────────────────────────────────────────
#define EEPROM_MAGIC 0xAB
void loadBest(){
  if(EEPROM.read(1) == EEPROM_MAGIC)
    best = EEPROM.read(0);
  else
    best = 0;
}
void saveBest(){
  EEPROM.write(0, best);
  EEPROM.write(1, EEPROM_MAGIC);
}

// ── ПСЕВДОСЛУЧАЙНЫЙ ГЕНЕРАТОР ─────────────────────────────────────────
int rndState = 1234;
int rnd(int a, int b){
  rndState = rndState * 1103515245 + 12345;
  return a + abs(rndState) % (b - a + 1);
}

// ── СТАРТ ИГРЫ ────────────────────────────────────────────────────────
void startGame(){
  birdY    = 200;
  birdVel  = 0;
  pipeX    = W + 20;
  gapY     = 32;
  score    = 0;
  newRecord= false;
  pipeSpeed= 2;
  delayMs  = 25;
  deathAnim= 0;
  state    = PLAY;
}

// ── ВСПОМОГАТЕЛЬНЫЕ: рисунки ──────────────────────────────────────────

// Красивые трубы с шапкой
void drawPipe(int x, int topH, int botY){
  // Тело трубы
  d.fillRect(x+1, 0,        PIPE_W-2, topH,     SSD1306_WHITE);
  d.fillRect(x+1, botY,     PIPE_W-2, H-botY,   SSD1306_WHITE);
  // Шапка трубы (чуть шире)
  d.fillRect(x-1, topH-4,   PIPE_W+2, 4,        SSD1306_WHITE);
  d.fillRect(x-1, botY,     PIPE_W+2, 4,        SSD1306_WHITE);
  // Блик (линия светлее — инвертируем 1 пиксель)
  d.drawFastVLine(x+2, 1, topH-5, SSD1306_BLACK);
  d.drawFastVLine(x+2, botY+4, H-botY-4, SSD1306_BLACK);
}

// Земля — полоса внизу
void drawGround(){
  d.drawFastHLine(0, H-3, W, SSD1306_WHITE);
  for(byte i=0; i<W; i+=4)
    d.drawPixel(i, H-2, SSD1306_WHITE);
}

// Облака (движущиеся по счёту)
void drawClouds(){
  static int cx1 = 10, cx2 = 70, cx3 = 110;
  cx1 -= 1; cx2 -= 1; cx3 -= 1;
  if(cx1 < -20) cx1 = W+10;
  if(cx2 < -20) cx2 = W+10;
  if(cx3 < -20) cx3 = W+10;
  // Облако 1
  d.drawCircle(cx1,   8, 4, SSD1306_WHITE);
  d.drawCircle(cx1+5, 6, 5, SSD1306_WHITE);
  d.drawCircle(cx1+10,8, 4, SSD1306_WHITE);
  // Облако 2
  d.drawCircle(cx2,   5, 3, SSD1306_WHITE);
  d.drawCircle(cx2+4, 4, 4, SSD1306_WHITE);
  d.drawCircle(cx2+8, 5, 3, SSD1306_WHITE);
  // Облако 3
  d.drawCircle(cx3,   9, 3, SSD1306_WHITE);
  d.drawCircle(cx3+5, 7, 4, SSD1306_WHITE);
}

// ── SETUP ─────────────────────────────────────────────────────────────
void setup(){
  pinMode(BTN, INPUT_PULLUP);
  pinMode(BUZ, OUTPUT);
  digitalWrite(BUZ, HIGH); // инверт. → тихо

  d.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  d.setTextColor(SSD1306_WHITE);
  d.clearDisplay();
  d.display();

  loadBest();
  sndMenu();
}

// ── LOOP ──────────────────────────────────────────────────────────────
void loop(){
  buzUpdate();

  if(millis() - frameT < delayMs) return;
  frameT = millis();

  // Считываем кнопку
  bool nowBtn = digitalRead(BTN);
  btnClick = (lastBtn == HIGH && nowBtn == LOW);
  lastBtn  = nowBtn;

  // ════════════════════ МЕНЮ ════════════════════
  if(state == MENU){
    menuFrame++;

    // Птичка парит
    menuBirdV += 3;
    menuBirdY += menuBirdV >> 2;
    if(menuBirdY > 260){ menuBirdY = 260; menuBirdV = -30; }
    if(menuBirdY < 160){ menuBirdY = 160; menuBirdV = 0;  }

    if(btnClick){
      sndClick();
      startGame();
      return;
    }

    d.clearDisplay();

    // Фон — точечный паттерн
    for(byte x=0; x<W; x+=8)
      for(byte y=0; y<H; y+=8)
        d.drawPixel(x, y, SSD1306_WHITE);

    // Птичка в меню
    int mby = menuBirdY >> 3;
    if(menuBirdV < -5)      d.drawBitmap(10, mby, bird_up,  8, 8, SSD1306_WHITE);
    else if(menuBirdV > 5)  d.drawBitmap(10, mby, bird_down,8, 8, SSD1306_WHITE);
    else                    d.drawBitmap(10, mby, bird_mid, 8, 8, SSD1306_WHITE);

    // Рамка заголовка
    d.drawRoundRect(20, 4, 106, 20, 3, SSD1306_WHITE);
    d.fillRoundRect(21, 5, 104, 18, 3, SSD1306_BLACK);

    // Заголовок
    d.setTextSize(2);
    d.setCursor(24, 7);
    d.print(F("SubNaNo"));

    // Мигающий текст "PRESS"
    if((menuFrame >> 3) & 1){
      d.setTextSize(1);
      d.setCursor(26, 30);
      d.print(F("[ PRESS TO FLY ]"));
    }

    // Рекорд
    if(best > 0){
      d.drawBitmap(30, 44, heart, 8, 8, SSD1306_WHITE);
      d.setTextSize(1);
      d.setCursor(42, 46);
      d.print(F("BEST:"));
      d.print(best);
    }

    // Версия
    d.setCursor(90, 57);
    d.print(F("v1.0"));

    d.display();
  }

  // ════════════════════ ИГРА ════════════════════
  else if(state == PLAY){

    if(btnClick){
      birdVel = JUMP_VEL;
      sndJump();
    }

    // Физика (fixed point × 10)
    birdVel += GRAVITY;
    birdY   += birdVel;

    // Трубы
    pipeX -= pipeSpeed;

    // Очко
    if(pipeX <= BIRD_X - PIPE_W && pipeX > BIRD_X - PIPE_W - pipeSpeed){
      score++;
      sndScore();
      // Ускорение каждые 5 очков
      if(score % 5 == 0){
        if(pipeSpeed < 5) pipeSpeed++;
        if(delayMs > 18)  delayMs--;
      }
    }

    // Перезапуск трубы
    if(pipeX < -PIPE_W - 2){
      pipeX = W;
      gapY  = rnd(GAP_HALF + 4, H - GAP_HALF - 10);
    }

    int by = birdY / 10; // реальный Y птички

    // Коллизия
    bool hit = false;
    if(by < 0 || by > H - 11) hit = true; // пол/потолок

    if(pipeX < BIRD_X + 8 && pipeX + PIPE_W > BIRD_X){
      if(by < gapY - GAP_HALF || by + 7 > gapY + GAP_HALF) hit = true;
    }

    if(hit){
      if(score > best){
        best = score;
        newRecord = true;
        saveBest();
        sndRecord();
      } else {
        sndDie();
      }
      deathAnim = 0;
      state = OVER;
      dClicks = 0;
    }

    // ── Рисование ──
    d.clearDisplay();
    drawClouds();

    int topH = gapY - GAP_HALF;
    int botY = gapY + GAP_HALF;
    drawPipe(pipeX, topH, botY);
    drawGround();

    // Птичка
    if(birdVel < -10)      d.drawBitmap(BIRD_X, by, bird_up,  8, 8, SSD1306_WHITE);
    else if(birdVel > 10)  d.drawBitmap(BIRD_X, by, bird_down,8, 8, SSD1306_WHITE);
    else                   d.drawBitmap(BIRD_X, by, bird_mid, 8, 8, SSD1306_WHITE);

    // Счёт
    d.setTextSize(1);
    d.setCursor(2, 2);
    d.print(score);

    // Звёздочка при рекорде (во время игры)
    if(score > 0 && score == best){
      d.drawBitmap(W - 10, 2, star, 8, 8, SSD1306_WHITE);
    }
    
    d.display();
  }

  // ════════════════════ GAME OVER ════════════════════
  else if(state == OVER){
    deathAnim++;

    // Двойной клик
    if(btnClick){
      dClicks++;
      dClickT = millis();
      sndClick();
    }
    if(dClicks > 0 && millis() - dClickT > 350){
      if(dClicks == 1){ state = MENU; sndMenu(); }
      else             { startGame(); }
      dClicks = 0;
      return;
    }

    d.clearDisplay();

    // Задний план (трубы, птичка)
    int by = birdY / 10;
    if(deathAnim < 20){
      // Анимация падения
      birdY   += 15;
      birdVel += 20;
    }
    by = birdY / 10;
    if(by > H - 9) by = H - 9;

    // Фон-трубы
    int topH = gapY - GAP_HALF;
    int botY = gapY + GAP_HALF;
    drawPipe(pipeX, topH, botY);
    drawGround();
    d.drawBitmap(BIRD_X, by, bird_down, 8, 8, SSD1306_WHITE);

    // Затемнение (шахматка)
    if(deathAnim > 6){
      for(byte xi=0; xi<W; xi+=2)
        for(byte yi=0; yi<H; yi+=2)
          d.drawPixel(xi, yi, SSD1306_BLACK);
    }

    // ── Окно GAME OVER ──
    d.fillRoundRect(8, 8, 112, 52, 4, SSD1306_BLACK);
    d.drawRoundRect(8, 8, 112, 52, 4, SSD1306_WHITE);
    // Двойная рамка
    d.drawRoundRect(10, 10, 108, 50, 3, SSD1306_WHITE);

    // Заголовок
    d.setTextSize(1);
    d.setCursor(27, 12);
    d.print(F("* GAME OVER *"));

    // Разделитель
    d.drawFastHLine(12, 20, 104, SSD1306_WHITE);

    // Очки
    d.setCursor(14, 27);
    d.print(F("Score:"));
    d.setCursor(60, 27);
    d.print(score);

    // Рекорд
    d.setCursor(14, 37);
    if(newRecord){
      // Мигающая надпись NEW BEST!
      if((deathAnim >> 2) & 1){
        d.drawBitmap(57, 36, heart, 8, 8, SSD1306_WHITE);
        d.setCursor(67, 37);
        d.print(F("NEW!"));
      }
      d.print(F("Best:"));
    } else {
      d.print(F("Best:"));
      d.setCursor(60, 37);
      d.print(best);
    }

    // Разделитель
    d.drawFastHLine(12, 47, 104, SSD1306_WHITE);

    // Подсказки
    d.setCursor(14, 50);
    d.print(F("1x:menu"));
    d.setCursor(68, 50);
    d.print(F("2x:retry"));

    d.display();
  }
}
