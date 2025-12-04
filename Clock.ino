#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>
#include <esp_timer.h> // タイマー割り込み用
#include "credentials.h" // Wi-Fi認証情報

// ==========================================
// 設定 (Wi-Fi)
// ==========================================
// WIFI_SSID と WIFI_PASS は credentials.h で定義されています
const char *NTP_SERVER = "ntp.nict.jp";
const long GMT_OFFSET_SEC = 9 * 3600; // JST
const int DAYLIGHT_OFFSET_SEC = 0;

// ==========================================
// デザインシステム定義
// ==========================================
// グリッド定数
const int CELL = 20;
const int COLS = 16;
const int ROWS = 12;

// カラーパレット (RGB565)
#define C_BG 0x0000     // Black
#define C_GRID 0x18E3   // Dark Gray
#define C_TEXT 0xFFFF   // White
#define C_DIM 0x8410    // Dim Gray
#define C_DIMMER 0x3186 // Darker Gray
#define C_ACCENT 0x07FF // Cyan
#define C_ALERT 0xFD20  // Orange
#define C_RELAX 0x07E0  // Green
#define C_EDIT 0xFBE0   // Amber

// フォント
const auto &F_TINY = fonts::Font0;
const auto &F_LABEL = fonts::Font2;
const auto &F_MEDIUM = fonts::Font4;
const auto &F_DIGIT = fonts::Font7;

// ==========================================
// グローバル変数・状態
// ==========================================
LGFX_Sprite canvas(&M5.Display);

enum AppMode
{
  MODE_CLOCK = 0,
  MODE_POMODORO,
  MODE_TIMER,
  MODE_MAX
};
AppMode currentMode = MODE_CLOCK;

// 割り込み間で共有する変数は volatile にする
struct AlarmState
{
  int h = 7;
  int m = 0;
  bool enabled = false;
  bool editing = false;
  // サウンド再生リクエスト用フラグ (ISRで立ててLoopで処理)
  volatile bool requestRing = false;
} alarmState;

struct PomoPhase
{
  int durationMin;
  bool isWork;
};
const PomoPhase POMO_CYCLE[] = {
    {15, true}, {5, false}, {30, true}, {5, false}, {45, true}, {5, false}, {60, true}, {5, false}};
struct PomoState
{
  int idx = 0;
  bool running = false;
  volatile float timeLeft = 15 * 60;     // ISRで変更される
  volatile bool requestFinished = false; // 終了音リクエスト
} pomo;

struct TimerState
{
  int setM = 3;
  int setS = 0;
  volatile float timeLeft = 3 * 60; // ISRで変更される
  bool running = false;
  bool finished = false;
  bool editing = false;
  volatile bool requestFinished = false; // 終了音リクエスト
} tmr;

// タイマー割り込みハンドル
esp_timer_handle_t logic_timer;

// ボタン連打制御用
unsigned long btnAPressTime = 0;
unsigned long btnALastRepeat = 0;
unsigned long btnBPressTime = 0;
unsigned long btnBLastRepeat = 0;
const int REPEAT_DELAY = 400;
const int REPEAT_INTERVAL = 100;

// ==========================================
// 関数プロトタイプ
// ==========================================
void connectWiFi();
void syncTime();
void handleInput();
void IRAM_ATTR onTimer(void *arg); // 割り込みハンドラ
void checkSoundRequests();         // メインループでの音再生
void draw();
void drawGrid();
void drawCell(int x, int y, int w, int h, String text, const lgfx::IFont *font, uint16_t color, String align = "center", float scale = 1.0);
void drawHugeTime(int h_val, int m_val, uint16_t color);
void drawBtnLabel(int x, String btn, String label);
void drawAlarmIcon(int col, int row, bool active);
void drawDot(int col, int row, uint16_t color);
void drawBorderHighlight(uint16_t color);

void drawClockMode();
void drawPomodoroMode();
void drawTimerMode();

// ==========================================
// 割り込み処理 (IRAM_ATTR: RAM上に配置して高速化)
// ==========================================
void IRAM_ATTR onTimer(void *arg)
{
  // 固定ステップ 0.01秒 (10ms)
  const float dt = 0.01f;

  // --- アラーム判定 ---
  // 秒単位の厳密な判定は難しいので、毎秒000msのタイミングを検知するのはメインループに任せるか、
  // ここでフラグを立てる。今回は簡易的にメインループで時刻チェックを行うためここではスキップするか、
  // 正確さを求めるならここで時刻更新フラグを立てるのがベスト。
  // (今回はWiFi同期のシステム時刻を使うため、アラーム判定はメインループの時刻取得時に行うのが安全)

  // --- ポモドーロ ---
  if (pomo.running && pomo.timeLeft > 0)
  {
    pomo.timeLeft -= dt;
    if (pomo.timeLeft <= 0)
    {
      pomo.timeLeft = 0;
      pomo.requestFinished = true; // メインループに通知
      // 次のフェーズへの移行はメインループで行う（配列アクセス等の安全のため）
      pomo.running = false; // 一旦停止
    }
  }

  // --- タイマー ---
  if (tmr.running && tmr.timeLeft > 0)
  {
    tmr.timeLeft -= dt;
    if (tmr.timeLeft <= 0)
    {
      tmr.timeLeft = 0;
      tmr.running = false;
      tmr.finished = true;
      tmr.requestFinished = true; // メインループに通知
    }
  }
}

// ==========================================
// セットアップ
// ==========================================
void setup()
{
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);

  // スプライト
  canvas.setColorDepth(8);
  if (canvas.createSprite(M5.Display.width(), M5.Display.height()) == nullptr)
  {
    M5.Display.println("Sprite create failed");
    return;
  }
  canvas.setTextWrap(false);

  // 起動画面
  canvas.fillScreen(C_BG);
  drawGrid();
  drawCell(0, 5, 16, 2, "CONNECTING WI-FI...", &F_MEDIUM, C_ACCENT, "center");
  canvas.pushSprite(0, 0);

  connectWiFi();
  syncTime();

  // --- タイマー割り込み設定 ---
  const esp_timer_create_args_t timer_args = {
      .callback = &onTimer,
      .name = "logic_timer"};
  esp_timer_create(&timer_args, &logic_timer);
  // 10ms = 10000マイクロ秒ごとに呼び出し
  esp_timer_start_periodic(logic_timer, 10000);
}

void connectWiFi()
{
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 10)
  {
    delay(500);
    retry++;
  }
}

void syncTime()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  }
}

// ==========================================
// メインループ
// ==========================================
void loop()
{
  M5.update();

  handleInput();
  checkSoundRequests(); // 割り込みからの通知を処理

  // アラーム時刻チェック (メインループで実行)
  if (currentMode == MODE_CLOCK && alarmState.enabled && !alarmState.editing)
  {
    struct tm t;
    getLocalTime(&t);
    static int lastSec = -1;
    if (t.tm_sec != lastSec)
    { // 1秒に1回だけチェック
      lastSec = t.tm_sec;
      if (t.tm_hour == alarmState.h && t.tm_min == alarmState.m && t.tm_sec == 0)
      {
        M5.Speaker.tone(2000, 500);
      }
    }
  }

  draw();
  canvas.pushSprite(0, 0);

  // delay(10) を削除し、最速で回す（割り込みがロジックを担保するため）
  // ただし、CPU負荷を少し下げるために1msだけ待つのが行儀が良い
  delay(1);
}

// ==========================================
// サウンド・状態遷移処理 (メインスレッド)
// ==========================================
void checkSoundRequests()
{
  // Pomodoro終了通知
  if (pomo.requestFinished)
  {
    pomo.requestFinished = false;
    M5.Speaker.tone(1000, 1000);

    // 次のサイクルへ (ロジック更新をここで行う)
    pomo.idx = (pomo.idx + 1) % (sizeof(POMO_CYCLE) / sizeof(POMO_CYCLE[0]));
    pomo.timeLeft = POMO_CYCLE[pomo.idx].durationMin * 60;
    // 自動継続したい場合はここで pomo.running = true;
    // 今回は手動スタート待ち（停止状態）のまま
  }

  // Timer終了通知
  if (tmr.requestFinished)
  {
    tmr.requestFinished = false;
    M5.Speaker.tone(1500, 800);
  }
}

// ==========================================
// 入力処理 (長押し連打対応)
// ==========================================
void handleInput()
{
  bool isEditingClock = (currentMode == MODE_CLOCK && alarmState.editing);
  bool isEditingTimer = (currentMode == MODE_TIMER && tmr.editing);
  bool allowRepeat = isEditingClock || isEditingTimer;

  // --- Button A ---
  bool btnATriggered = false;
  if (M5.BtnA.wasPressed())
  {
    btnATriggered = true;
    btnAPressTime = millis();
  }
  else if (allowRepeat && M5.BtnA.isPressed())
  {
    if (millis() - btnAPressTime > REPEAT_DELAY)
    {
      if (millis() - btnALastRepeat > REPEAT_INTERVAL)
      {
        btnATriggered = true;
        btnALastRepeat = millis();
      }
    }
  }

  if (btnATriggered)
  {
    if (isEditingClock)
    {
      alarmState.h = (alarmState.h + 1) % 24;
      M5.Speaker.tone(6000, 10);
    }
    else if (isEditingTimer)
    {
      tmr.setM = (tmr.setM + 1) % 100;
      M5.Speaker.tone(6000, 10);
    }
    else
    {
      if (M5.BtnA.wasPressed())
      {
        currentMode = (AppMode)((currentMode + 1) % MODE_MAX);
        M5.Speaker.tone(4000, 20);
      }
    }
  }

  // --- Button B ---
  bool btnBTriggered = false;
  if (M5.BtnB.wasPressed())
  {
    btnBTriggered = true;
    btnBPressTime = millis();
  }
  else if (allowRepeat && M5.BtnB.isPressed())
  {
    if (millis() - btnBPressTime > REPEAT_DELAY)
    {
      if (millis() - btnBLastRepeat > REPEAT_INTERVAL)
      {
        btnBTriggered = true;
        btnBLastRepeat = millis();
      }
    }
  }

  if (btnBTriggered)
  {
    if (isEditingClock)
    {
      alarmState.m = (alarmState.m + 1) % 60;
      M5.Speaker.tone(6000, 10);
    }
    else if (isEditingTimer)
    {
      tmr.setS = (tmr.setS + 1) % 60; // 1秒単位
      M5.Speaker.tone(6000, 10);
    }
    else
    {
      if (M5.BtnB.wasPressed())
      {
        if (currentMode == MODE_CLOCK)
          alarmState.enabled = !alarmState.enabled;
        else if (currentMode == MODE_POMODORO)
          pomo.running = !pomo.running;
        else if (currentMode == MODE_TIMER)
        {
          if (tmr.finished)
          {
            tmr.finished = false;
            tmr.timeLeft = tmr.setM * 60 + tmr.setS;
          }
          else
            tmr.running = !tmr.running;
        }
        M5.Speaker.tone(4000, 20);
      }
    }
  }

  if (currentMode == MODE_TIMER && M5.BtnB.pressedFor(800))
  {
    if (!tmr.running && !tmr.editing)
    {
      tmr.timeLeft = tmr.setM * 60 + tmr.setS;
      tmr.finished = false;
      M5.Speaker.tone(2000, 100);
    }
  }

  // --- Button C ---
  if (M5.BtnC.wasPressed())
  {
    if (currentMode == MODE_CLOCK)
    {
      alarmState.editing = !alarmState.editing;
    }
    else if (currentMode == MODE_POMODORO)
    {
      pomo.running = false;
      pomo.idx = 0;
      pomo.timeLeft = POMO_CYCLE[0].durationMin * 60;
    }
    else if (currentMode == MODE_TIMER)
    {
      if (tmr.editing)
      {
        tmr.editing = false;
        tmr.timeLeft = tmr.setM * 60 + tmr.setS;
        tmr.finished = false;
      }
      else if (!tmr.running)
      {
        tmr.editing = true;
      }
    }
    M5.Speaker.tone(4000, 20);
  }
}

// ==========================================
// 描画ヘルパー関数
// ==========================================

int gx(float x) { return (int)(x * CELL); }
int gy(float y) { return (int)(y * CELL); }

void drawGrid()
{
  for (int i = 1; i < COLS; i++)
  {
    canvas.drawFastVLine(gx(i), 0, canvas.height(), C_GRID);
  }
  for (int j = 1; j < ROWS; j++)
  {
    canvas.drawFastHLine(0, gy(j), canvas.width(), C_GRID);
  }
}

void drawCell(int x, int y, int w, int h, String text, const lgfx::IFont *font, uint16_t color, String align, float scale)
{
  int px = gx(x);
  int py = gy(y);
  int pw = gx(w);
  int ph = gy(h);

  canvas.setFont(font);
  canvas.setTextColor(color);
  canvas.setTextSize(scale);

  int tx = px;
  int ty = py + ph / 2;

  if (align == "center")
  {
    canvas.setTextDatum(middle_center);
    tx += pw / 2;
    if (font == &F_DIGIT)
      ty += 2;
    else
      ty -= 2;
  }
  else if (align == "right")
  {
    canvas.setTextDatum(middle_right);
    tx += pw - 4;
    if (font == &F_DIGIT)
      ty += 2;
    else
      ty -= 2;
  }
  else
  {
    canvas.setTextDatum(middle_left);
    tx += 4;
    if (font == &F_DIGIT)
      ty += 2;
    else
      ty -= 2;
  }

  canvas.drawString(text, tx, ty);
  canvas.setTextSize(1.0);
}

void drawDot(int col, int row, uint16_t color)
{
  canvas.fillCircle(gx(col), gy(row) - 5, 2, color);
}

void drawAlarmIcon(int col, int row, bool active)
{
  int cx = gx(col) + CELL / 2;
  int cy = gy(row) + CELL / 2;
  uint16_t color = active ? C_TEXT : C_DIM;

  canvas.drawArc(cx, cy - 1, 5, 0, 180, 360, color);
  canvas.drawLine(cx + 5, cy - 1, cx + 6, cy + 5, color);
  canvas.drawLine(cx - 5, cy - 1, cx - 6, cy + 5, color);
  canvas.drawLine(cx - 6, cy + 5, cx + 6, cy + 5, color);

  if (active)
  {
    canvas.fillArc(cx, cy - 1, 4, 0, 180, 360, color);
    canvas.fillTriangle(cx, cy - 1, cx - 5, cy + 4, cx + 5, cy + 4, color);
  }
  canvas.fillCircle(cx, cy + 6, 2, color);
}

void drawHugeTime(int h_val, int m_val, uint16_t color)
{
  float scale = 2.0;
  int y_base = gy(5);

  canvas.setFont(&F_DIGIT);
  canvas.setTextColor(color);
  canvas.setTextSize(scale);
  canvas.setTextDatum(middle_center);

  int y = y_base + 8;

  canvas.drawString(String(h_val / 10), gx(2.5), y);
  canvas.drawString(String(h_val % 10), gx(5.5), y);

  canvas.fillRect(gx(8) - 3, y - 18, 6, 6, C_DIM);
  canvas.fillRect(gx(8) - 3, y + 12, 6, 6, C_DIM);

  canvas.drawString(String(m_val / 10), gx(10.5), y);
  canvas.drawString(String(m_val % 10), gx(13.5), y);

  canvas.setTextSize(1.0);
}

void drawBtnLabel(int x, String btn, String label)
{
  if (label == "")
    return;
  drawCell(x, 10, 1, 1, btn, &F_LABEL, C_ACCENT, "center");
  drawCell(x + 1, 10, 3, 1, label, &F_LABEL, C_DIM, "center");
}

void drawBorderHighlight(uint16_t color)
{
  // 外周1マス分をベタ塗り
  canvas.fillRect(0, 0, canvas.width(), CELL, color);                                    // 上
  canvas.fillRect(0, canvas.height() - CELL, canvas.width(), CELL, color);               // 下
  canvas.fillRect(0, CELL, CELL, canvas.height() - 2 * CELL, color);                     // 左
  canvas.fillRect(canvas.width() - CELL, CELL, CELL, canvas.height() - 2 * CELL, color); // 右

  // 境界線強調
  canvas.drawFastHLine(0, CELL, canvas.width(), C_GRID);
  canvas.drawFastHLine(0, canvas.height() - CELL, canvas.width(), C_GRID);
  canvas.drawFastVLine(CELL, 0, canvas.height(), C_GRID);
  canvas.drawFastVLine(canvas.width() - CELL, 0, canvas.height(), C_GRID);
}

// ==========================================
// 描画メイン
// ==========================================
void draw()
{
  canvas.fillScreen(C_BG);
  drawGrid();

  bool highlight = false;
  uint16_t hlColor = C_ACCENT;

  struct tm t;
  getLocalTime(&t);

  if (currentMode == MODE_CLOCK)
  {
    if (t.tm_min == 0 || t.tm_min == 30)
      highlight = true;
  }
  else if (currentMode == MODE_POMODORO)
  {
    if (!POMO_CYCLE[pomo.idx].isWork && pomo.running)
    {
      highlight = true;
      hlColor = C_RELAX;
    }
  }
  else if (currentMode == MODE_TIMER)
  {
    if (tmr.finished)
    {
      highlight = true;
      hlColor = C_ALERT;
    }
  }

  // Header
  drawCell(1, 1, 2, 1, "MODE", &F_LABEL, C_ACCENT, "left");
  String modeLabels[] = {"CLOCK", "POMODORO", "TIMER"};
  String modeStr = modeLabels[currentMode];
  if (currentMode == MODE_CLOCK && alarmState.editing)
    modeStr = "SET ALARM";
  drawCell(3, 1, 3, 1, modeStr, &F_LABEL, alarmState.editing ? C_EDIT : C_TEXT, "left");

  // Status
  String statusStr = "";
  uint16_t statusColor = C_DIM;
  if (currentMode == MODE_CLOCK)
  {
    char buf[16];
    sprintf(buf, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    statusStr = String(buf);
  }
  else if (currentMode == MODE_POMODORO)
  {
    statusStr = POMO_CYCLE[pomo.idx].isWork ? "WORK" : "BREAK";
    statusColor = POMO_CYCLE[pomo.idx].isWork ? C_ALERT : C_RELAX;
  }
  else if (currentMode == MODE_TIMER)
  {
    if (tmr.finished)
    {
      statusStr = "TIME UP";
      statusColor = C_ALERT;
    }
    else
      statusStr = tmr.running ? "RUNNING" : "STOPPED";
  }
  drawCell(6, 1, 4, 1, statusStr, &F_LABEL, statusColor, "center");

  // Right Info
  if (currentMode == MODE_CLOCK)
  {
    drawAlarmIcon(10, 1, alarmState.enabled);
    drawCell(11, 1, 2, 1, "ALM", &F_LABEL, C_TEXT, "center");
    char buf[8];
    sprintf(buf, "%02d:%02d", alarmState.h, alarmState.m);
    drawCell(13, 1, 2, 1, buf, &F_LABEL, C_ACCENT, "center");
  }
  else
  {
    // 割り込み中はmillis()で描画補完
    int ms = (millis() % 1000) / 10;
    char buf[16];
    sprintf(buf, "%02d:%02d:%02d.%02d", t.tm_hour, t.tm_min, t.tm_sec, ms);
    drawCell(11, 1, 4, 1, buf, &F_LABEL, C_DIM, "right");
  }

  canvas.fillRect(gx(1), gy(2), gx(14), 2, C_GRID);
  canvas.fillRect(gx(1), gy(10), gx(14), 1, C_GRID);

  switch (currentMode)
  {
  case MODE_CLOCK:
    drawClockMode();
    break;
  case MODE_POMODORO:
    drawPomodoroMode();
    break;
  case MODE_TIMER:
    drawTimerMode();
    break;
  }

  String la = "MODE", lb = "", lc = "";
  if (currentMode == MODE_CLOCK)
  {
    if (alarmState.editing)
    {
      la = "HOUR+";
      lb = "MIN+";
      lc = "OK";
    }
    else
    {
      lb = alarmState.enabled ? "ALM OFF" : "ALM ON";
      lc = "SET ALM";
    }
  }
  else if (currentMode == MODE_POMODORO)
  {
    lb = pomo.running ? "PAUSE" : "START";
    lc = "RESET";
  }
  else if (currentMode == MODE_TIMER)
  {
    if (tmr.editing)
    {
      la = "MIN+";
      lb = "SEC+";
      lc = "OK";
    }
    else
    {
      lb = tmr.running ? "PAUSE" : "START";
      lc = "SET";
    }
  }

  drawBtnLabel(1, "A", la);
  drawBtnLabel(6, "B", lb);
  drawBtnLabel(11, "C", lc);

  drawCell(5, 10, 1, 1, "/", &F_LABEL, C_DIM, "center");
  drawCell(10, 10, 1, 1, "/", &F_LABEL, C_DIM, "center");

  if (highlight)
    drawBorderHighlight(hlColor);
}

void drawClockMode()
{
  struct tm t;
  getLocalTime(&t);
  int h = alarmState.editing ? alarmState.h : t.tm_hour;
  int m = alarmState.editing ? alarmState.m : t.tm_min;

  uint16_t mainColor = alarmState.editing ? C_EDIT : C_TEXT;
  drawHugeTime(h, m, mainColor);

  if (alarmState.editing)
  {
    drawCell(1, 8, 14, 2, ":: ALARM EDIT MODE ::", &F_MEDIUM, C_EDIT, "center");
  }
  else
  {
    int Y = t.tm_year + 1900;
    int M = t.tm_mon + 1;
    int D = t.tm_mday;
    int s = t.tm_sec;
    int ms = (millis() % 1000) / 10;
    const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

    String ys = String(Y);
    for (int i = 0; i < 4; i++)
      drawCell(1 + i, 8, 1, 2, String(ys[i]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawDot(5, 10, C_DIMMER);

    String ms_str = (M < 10 ? "0" : "") + String(M);
    drawCell(5, 8, 1, 2, String(ms_str[0]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawCell(6, 8, 1, 2, String(ms_str[1]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawDot(7, 10, C_DIMMER);

    String ds = (D < 10 ? "0" : "") + String(D);
    drawCell(7, 8, 1, 2, String(ds[0]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawCell(8, 8, 1, 2, String(ds[1]), &F_DIGIT, C_TEXT, "center", 0.6);

    drawCell(9, 8, 2, 2, wd[t.tm_wday], &F_TINY, C_DIM, "center", 2.0);

    String ss = (s < 10 ? "0" : "") + String(s);
    drawCell(11, 8, 1, 2, String(ss[0]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawCell(12, 8, 1, 2, String(ss[1]), &F_DIGIT, C_TEXT, "center", 0.6);
    drawDot(13, 10, C_DIMMER);

    String mss = (ms < 10 ? "0" : "") + String(ms);
    drawCell(13, 8, 1, 2, String(mss[0]), &F_DIGIT, C_DIM, "center", 0.6);
    drawCell(14, 8, 1, 2, String(mss[1]), &F_DIGIT, C_DIM, "center", 0.6);
  }
}

void drawPomodoroMode()
{
  int m = (int)pomo.timeLeft / 60;
  int s = (int)pomo.timeLeft % 60;
  drawHugeTime(m, s, C_TEXT);

  float total = POMO_CYCLE[pomo.idx].durationMin * 60.0;
  float pct = 1.0 - (pomo.timeLeft / total);
  uint16_t color = POMO_CYCLE[pomo.idx].isWork ? C_ALERT : C_RELAX;

  canvas.fillRect(gx(1), gy(8), gx(14), gy(1), C_GRID);
  canvas.fillRect(gx(1), gy(8), (int)(gx(14) * pct), gy(1), color);

  char buf[32];
  sprintf(buf, "CYCLE %d/%d", pomo.idx + 1, sizeof(POMO_CYCLE) / sizeof(PomoPhase));
  drawCell(1, 9, 14, 1, buf, &F_LABEL, C_DIM, "center");
}

void drawTimerMode()
{
  uint16_t color = tmr.editing ? C_EDIT : (tmr.finished ? C_ALERT : C_TEXT);
  if (tmr.finished && (millis() % 500 < 250))
    color = C_BG;

  int totalSec = tmr.editing ? (tmr.setM * 60 + tmr.setS) : (int)ceil(tmr.timeLeft);
  drawHugeTime(totalSec / 60, totalSec % 60, color);

  if (tmr.editing)
  {
    drawCell(1, 8, 14, 2, ":: EDIT MODE ::", &F_MEDIUM, C_DIM, "center");
  }
  else
  {
    char buf[32];
    sprintf(buf, "SET: %02d:%02d", tmr.setM, tmr.setS);
    drawCell(1, 8, 14, 2, buf, &F_MEDIUM, C_DIM, "center");
  }
}