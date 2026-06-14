#include "HX711.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"


#include <WebServer.h>

WebServer camServer(81);  // port 81, tách khỏi port 80 nếu sau này dùng
TaskHandle_t cameraStreamTaskHandle = NULL;



// TaskHandle_t cameraStreamTaskHandle = NULL;

// ── WiFi ─────────────────────────────────────────
const char* WIFI_SSID = "Bo De";
const char* WIFI_PASS = "13579999";
const char* SERVER_URL = "http://192.168.1.23:8000/predict/";
const char* SESSION_URL = "http://192.168.1.23:8000/session/status/";

struct SessionStatus {
  bool active = false;
  bool locked = false;
  int rescanCount = 0;
};

// ════════════════════════════════════════════════
// DEBUG LOGGER
// ════════════════════════════════════════════════
#define DEBUG_ENABLED 1

#if DEBUG_ENABLED
#define LOG_SYS(msg) Serial.printf("[SYS ] %s\n", msg)
#define LOG_INFO(fmt, ...) Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_OK(fmt, ...) Serial.printf("[OK   ] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) Serial.printf("[WARN ] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Serial.printf("[ERR ] " fmt "\n", ##__VA_ARGS__)
#define LOG_SEP() Serial.println("[SYS ] ----------------------------")
#else
#define LOG_SYS(msg)
#define LOG_INFO(fmt, ...)
#define LOG_OK(fmt, ...)
#define LOG_WARN(fmt, ...)
#define LOG_ERR(fmt, ...)
#define LOG_SEP()
#endif

// ── Pin definitions ───────────────────────────────
#define HX_DT 41
#define HX_SCK 42
#define I2C_SDA 39
#define I2C_SCL 40
#define US_TRIG 14
#define US_ECHO 21
#define LED_R 47
#define LED_G 48
#define LED_B 38
#define BUZZER 3

// ── Camera pins ──────────────────────────────────
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

// ── Cấu hình ──────────────────────────────────────
const float CALIBRATION_FACTOR = 1141288.611f;
const float ALPHA = 0.5f;
const float DEADBAND_KG = 0.001f;
const float US_THRESHOLD_CM = 15.0f;
const float WEIGHT_MIN_KG = 0.005f;
const float STABLE_THRESHOLD = 0.005f;
const int STABLE_COUNT_MAX = 6;

// ── Objects ───────────────────────────────────────
HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── State ─────────────────────────────────────────
enum State { IDLE,
             WEIGHING,
             STABLE,
             SENDING,
             RESULT,
             LOCKED };
State state = IDLE;

float ema = 0;
float displayed = 0;
float lastStable = 0;
int stableCount = 0;
bool objectPresent = false;
// Thêm biến này vào phần State
bool stableBeepDone = false;
int currentRescanCount = 0;
bool lastActive = false; // Theo dõi trạng thái phiên cân để cập nhật LCD

// ── Buzzer ────────────────────────────────────────
unsigned long beepEnd = 0;
unsigned long beepPauseEnd = 0;
int beepRepeat = 0;
int beepDuration = 0;

// ── Helper functions ──────────────────────────────

void updateIdleDisplay(bool active) {
  if (active) {
    lcdPrint("  San sang can  ", "  Moi dat vat... ");
    setRGB(0, 0, 1); // Màu xanh dương - Sẵn sàng
  } else {
    lcdPrint("  Cho phien can ", "  Chua bat dau  ");
    setRGB(0, 0, 0); // Tắt LED
  }
}

void logState(float dist, float raw, float output, bool usDetect, bool wDetect) {
#if DEBUG_ENABLED
  const char* stateNames[] = { "IDLE", "WEIGHING", "STABLE", "SENDING", "RESULT", "LOCKED" };
  LOG_INFO("─── Sensor snapshot ───────────────────");
  LOG_INFO("  US dist     : %.1f cm  (detect=%s, threshold=%.0f cm)", dist, usDetect ? "YES" : "no", US_THRESHOLD_CM);
  LOG_INFO("  Raw weight  : %.4f kg", raw);
  LOG_INFO("  EMA         : %.4f kg", ema);
  LOG_INFO("  Output      : %.4f kg  (detect=%s, min=%.3f kg)", output, wDetect ? "YES" : "no", WEIGHT_MIN_KG);
  LOG_INFO("  State       : %s  | stableCount=%d/%d", stateNames[state], stableCount, STABLE_COUNT_MAX);
  LOG_INFO("  objectPres. : %s", objectPresent ? "YES" : "no");
#endif
}

void updateBuzzer() {
  unsigned long now = millis();
  if (digitalRead(BUZZER) == HIGH && now >= beepEnd) {
    digitalWrite(BUZZER, LOW);
    if (beepRepeat > 0) beepPauseEnd = now + 60;
  }
  if (digitalRead(BUZZER) == LOW && beepRepeat > 0 && now >= beepPauseEnd) {
    beepRepeat--;
    digitalWrite(BUZZER, HIGH);
    beepEnd = now + beepDuration;
  }
}

void startBeep(int ms, int count = 1) {
  beepDuration = ms;
  beepRepeat = count - 1;
  digitalWrite(BUZZER, HIGH);
  beepEnd = millis() + ms;
}

SessionStatus getSessionStatus() {
  SessionStatus status;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("WiFi not connected — cannot check session");
    return status;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, SESSION_URL);
  http.setTimeout(5000); // Tăng timeout lên 5s
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    status.active = (resp.indexOf("\"active\":true") >= 0 || resp.indexOf("\"active\": true") >= 0);
    status.locked = (resp.indexOf("\"locked\":true") >= 0 || resp.indexOf("\"locked\": true") >= 0);
    
    int idx = resp.indexOf("\"rescan_count\"");
    if (idx >= 0) {
      int s = resp.indexOf(":", idx) + 1;
      while (s < resp.length() && !isdigit(resp[s])) s++;
      int e = s;
      while (e < resp.length() && isdigit(resp[e])) e++;
      status.rescanCount = resp.substring(s, e).toInt();
    }
    LOG_INFO("Session response: active=%d, locked=%d, rescan=%d", status.active, status.locked, status.rescanCount);
  } else {
    LOG_ERR("Failed to check session — HTTP code: %d", code);
  }
  http.end();
  return status;
}

bool isSessionActive() {
  SessionStatus status = getSessionStatus();
  return status.active;
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;                    // ← đổi từ 1 thành 2
  config.grab_mode = CAMERA_GRAB_LATEST;  // ← thêm dòng này

  LOG_INFO("Calling esp_camera_init...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOG_ERR("Camera init failed: 0x%x", err);
    if (err == 0x20001) LOG_ERR("→ No memory — PSRAM chưa đủ");
    if (err == 0x101) LOG_ERR("→ Sensor timeout — kiểm tra dây cáp camera");
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  LOG_INFO("Sensor PID: 0x%02X (OV2640=0x26)", s->id.PID);
  s->set_whitebal(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
  return true;
}

void handleMjpegStream() {
  WiFiClient client = camServer.client();

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();

  LOG_OK("MJPEG client: %s", client.remoteIP().toString().c_str());

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    // yield để FreeRTOS không bị watchdog
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  LOG_OK("MJPEG client disconnected");
}

void handleNotFound() {
  camServer.send(404, "text/plain", "Not found. Try /stream");
}

void cameraStreamTask(void* parameter) {
  camServer.on("/stream", HTTP_GET, handleMjpegStream);
  camServer.onNotFound(handleNotFound);
  camServer.begin();

  LOG_OK("MJPEG server started → http://%s:81/stream",
         WiFi.localIP().toString().c_str());

  while (true) {
    camServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

float getMedianWeight(int n) {
  float buf[n];
  for (int i = 0; i < n; i++) {
    buf[i] = scale.get_units(1);
    delay(8);
  }
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (buf[j] > buf[j + 1]) {
        float t = buf[j];
        buf[j] = buf[j + 1];
        buf[j + 1] = t;
      }
  return buf[n / 2];
}

float readUS100() {
  digitalWrite(US_TRIG, LOW);
  delayMicroseconds(5);
  digitalWrite(US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG, LOW);
  long dur = pulseIn(US_ECHO, HIGH, 30000);
  return (dur == 0) ? -1.0f : dur * 0.0343f / 2.0f;
}

void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

void updateLCD(float kg) {
  lcd.setCursor(0, 0);
  lcd.print("  Khoi luong:   ");
  lcd.setCursor(0, 1);
  char buf[17];
  if (kg < 1.0f) snprintf(buf, sizeof(buf), "  %5.0f g       ", kg * 1000);
  else snprintf(buf, sizeof(buf), "  %5.3f kg      ", kg);
  lcd.print(buf);
}

void lcdPrint(const char* line0, const char* line1) {
  lcd.setCursor(0, 0);
  lcd.print(line0);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}

void idleBlink() {
  static unsigned long lastBlink = 0;
  static bool ledOn = false;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    ledOn = !ledOn;
    setRGB(0, 0, ledOn);
  }
}

bool sendToServer(float kg, String& outName, float& outPrice, float& outWeight) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("WiFi not connected — cannot send");
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  // Build URL với query params: weight + cam_ip
  String url = String(SERVER_URL)
               + "?weight=" + String(kg, 4)
               + "&cam_ip=" + WiFi.localIP().toString();

  LOG_INFO("Sending → %s", url.c_str());
  lcdPrint("  Dang gui...   ", " Vui long cho   ");
  setRGB(1, 1, 0);  // vàng = đang gửi

  LOG_INFO("Full URL: %s", url.c_str());

  http.begin(client, url);
  http.setTimeout(10000);
  int code = http.GET();

  LOG_INFO("HTTP response code: %d", code);

  if (code == 200) {
    String resp = http.getString();
    LOG_INFO("Response: %s", resp.c_str());
    http.end();

    // Parse JSON thủ công (không dùng thư viện)
    // "name"
    int i = resp.indexOf("\"name\":");
    if (i >= 0) {
      int s = resp.indexOf("\"", i + 7) + 1;
      int e = resp.indexOf("\"", s);
      outName = resp.substring(s, e);
    }

    // "price"
    i = resp.indexOf("\"price\":");
    if (i >= 0) {
      int s = i + 8;
      // bỏ qua space nếu có
      while (resp[s] == ' ') s++;
      int e = resp.indexOf(",", s);
      if (e < 0) e = resp.indexOf("}", s);
      outPrice = resp.substring(s, e).toFloat();
    }

    // "weight"
    i = resp.indexOf("\"weight\":");
    if (i >= 0) {
      int s = i + 9;
      while (resp[s] == ' ') s++;
      int e = resp.indexOf(",", s);
      if (e < 0) e = resp.indexOf("}", s);
      outWeight = resp.substring(s, e).toFloat();
    }

    LOG_OK("Parsed → name=%s | price=%.0f | weight=%.3f",
           outName.c_str(), outPrice, outWeight);
    return true;

  } else {
    LOG_ERR("HTTP failed — code: %d", code);
    http.end();
    return false;
  }
}

// ════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);

  LOG_SEP();
  LOG_SYS("Freenove ESP32-S3-WROOM CAM");
  LOG_INFO("Free heap  : %d bytes", ESP.getFreeHeap());
  LOG_INFO("PSRAM      : %s", psramFound() ? "FOUND" : "NOT FOUND");
  LOG_INFO("PSRAM size : %d bytes", ESP.getPsramSize());
  LOG_SEP();

  // ── GPIO ────────────────────────────────────────
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);
  setRGB(0, 0, 1);  // xanh dương = đang khởi động
  LOG_OK("GPIO initialized");

  // ── LCD ─────────────────────────────────────────
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcdPrint("  Can thong minh", " Dang khoi dong ");
  LOG_OK("LCD initialized");
 
  // ── WiFi (có timeout 15s) ───────────────────────
  LOG_INFO("WiFi connecting to: %s", WIFI_SSID);
  lcdPrint(" Dang ket noi...", "  WiFi...       ");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
 
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 15000) {
      LOG_WARN("WiFi timeout — continuing without WiFi");
      lcdPrint("  Het gio WiFi  ", " Khong co server");
      delay(1500);
      break;  // không crash, chạy tiếp ở chế độ offline
    }
    delay(300);
  }
 
  if (WiFi.status() == WL_CONNECTED) {
    LOG_OK("WiFi connected — IP: %s", WiFi.localIP().toString().c_str());
    lcdPrint("  WiFi OK       ", "                ");
    delay(500);
  }
 
  // ── Camera ──────────────────────────────────────
  LOG_INFO("Initializing camera...");
  lcdPrint(" Khoi tao Cam...", "                ");
 
  if (!psramFound()) {
    LOG_ERR("No PSRAM — camera disabled");
    LOG_ERR("Fix: Tools -> PSRAM -> OPI PSRAM");
    lcdPrint("  Khong PSRAM!  ", "  Bo qua Cam    ");
    delay(2000);
    // Tiếp tục chạy không có camera
  } else {
    if (initCamera()) {
      LOG_OK("Camera ready");
      lcdPrint("   Camera OK    ", "                ");
    } else {
      LOG_ERR("Camera init failed — check cable/pins");
      lcdPrint("   LOI CAMERA!  ", "  Kiem tra cap  ");
      delay(2000);
      // Không dừng hẳn, chạy tiếp không có camera
    }
    delay(500);
  }
 
  // ── HX711 ───────────────────────────────────────
  LOG_INFO("Initializing HX711...");
  lcdPrint(" Khoi tao can...", "                ");
  scale.begin(HX_DT, HX_SCK);
 
  unsigned long hxStart = millis();
  while (!scale.is_ready()) {
    if (millis() - hxStart > 5000) {
      LOG_ERR("HX711 timeout — check wiring DT=%d SCK=%d", HX_DT, HX_SCK);
      lcdPrint("   LOI HX711!   ", " Kiem tra day   ");
      while (true) delay(1000);
    }
    delay(50);
  }
  scale.set_scale(CALIBRATION_FACTOR);
  delay(1000);
 
  // ── Tare với miếng lót ──────────────────────────
  lcdPrint(" Dat mieng lot  ", " len can, cho...");
  LOG_INFO("Waiting 4s — place pad on scale...");
  delay(4000);
 
  scale.tare();
  LOG_OK("HX711 ready — tared with pad");
  lcdPrint("  Hieu chuan OK!", "                ");
  delay(800);

  // ── Done ────────────────────────────────────────
  LOG_SEP();
  LOG_OK("System ready!");
  LOG_INFO("State: IDLE — waiting for session/object");
  LOG_SEP();

  // Kiểm tra trạng thái ban đầu
  SessionStatus initStatus = getSessionStatus();
  lastActive = initStatus.active;
  currentRescanCount = initStatus.rescanCount;
  updateIdleDisplay(lastActive);

  // ── MJPEG Camera Stream Task ─────────────────────
  xTaskCreatePinnedToCore(
    cameraStreamTask,
    "CamStream",
    8192,
    NULL,
    2,
    &cameraStreamTaskHandle,
    1  // Core 1
  );
  LOG_OK("MJPEG stream task created on Core 1");
}

// ════════════════════════════════════════════════
// Loop
// ════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Buzzer luôn update, không phụ thuộc vào timing loop ──
  updateBuzzer();

  // ── Giới hạn tốc độ vòng loop 150ms ──────────────────────
  static unsigned long lastLoop = 0;
  if (now - lastLoop < 150) return;
  lastLoop = now;

  // ── Đọc cảm biến ─────────────────────────────────────────
  float dist = readUS100();
  bool usDetect = (dist > 0 && dist < US_THRESHOLD_CM);

  float raw = getMedianWeight(7);
  if (abs(raw - ema) > 0.050f) ema = raw;
  else ema = ALPHA * raw + (1 - ALPHA) * ema;

  float output = (abs(ema) < 0.0005f) ? 0 : ema;
  if (output < 0) output = 0;

  bool weightDetect = (output > WEIGHT_MIN_KG);
  objectPresent = usDetect && weightDetect;

  static unsigned long lastSnapshot = 0;
  if (state == IDLE && now - lastSnapshot > 2000) {
    lastSnapshot = now;
    logState(dist, raw, output, usDetect, weightDetect);
  }

  switch (state) {
    case IDLE:
      idleBlink();

      // Định kỳ kiểm tra session status để phát hiện LOCKED và lấy rescan_count hiện tại
      static unsigned long lastSessionCheck = 0;
      if (now - lastSessionCheck > 2000) {
        lastSessionCheck = now;
        SessionStatus sStatus = getSessionStatus();
        
        // Luôn cập nhật rescanCount
        currentRescanCount = sStatus.rescanCount;

        if (sStatus.locked) {
          state = LOCKED;
          lcdPrint("  HE THONG KHOA ", "  Cho nhan vien..");
          setRGB(1, 0, 0);
          LOG_WARN("System is LOCKED by server!");
          break;
        }

        // Luôn cập nhật LCD dựa trên trạng thái Active thực tế từ server
        if (sStatus.active != lastActive) {
          lastActive = sStatus.active;
          updateIdleDisplay(lastActive);
        } else {
          // Trường hợp khởi động xong, lastActive có thể khớp nhưng LCD chưa hiện đúng
          // (Dù setup đã gọi, nhưng để chắc chắn ta có thể refresh định kỳ hoặc dựa trên flag)
        }
      }

      // Chỉ bắt đầu cân nếu phiên đang Active
      if (lastActive && usDetect && weightDetect) {
        LOG_SEP();
        LOG_OK("Object detected → US=%.1f cm | weight=%.4f kg", dist, output);
        stableCount = 0;
        lastStable = output;
        stableBeepDone = false;
        setRGB(0, 1, 0);
        state = WEIGHING;
      }
      break;

    case WEIGHING:
      if (!usDetect && !weightDetect) {
        ema = displayed = 0;
        stableCount = 0;
        stableBeepDone = false;
        updateIdleDisplay(lastActive);
        state = IDLE;
        LOG_OK("Object removed → IDLE");
        break;
      }
      if (abs(output - displayed) > DEADBAND_KG) {
        displayed = output;
        updateLCD(displayed);
      }
      if (abs(output - lastStable) < STABLE_THRESHOLD) {
        stableCount++;
        LOG_INFO("Stable: %d/%d | diff=%.1fg",
                 stableCount, STABLE_COUNT_MAX,
                 abs(output - lastStable) * 1000);
        if (stableCount >= STABLE_COUNT_MAX) {
          state = STABLE;
        }
      } else {
        LOG_WARN("Reset | diff=%.1fg", abs(output - lastStable) * 1000);
        stableCount = 0;
        lastStable = output;
      }
      break;

    case STABLE:
      if (!usDetect && !weightDetect) {
        ema = displayed = 0;
        stableCount = 0;
        stableBeepDone = false;
        updateIdleDisplay(lastActive);
        state = IDLE;
        LOG_OK("Object removed at STABLE → IDLE");
        break;
      }

      // Beep đúng 1 lần khi mới vào STABLE
      if (!stableBeepDone) {
        stableBeepDone = true;
        digitalWrite(BUZZER, LOW);
        beepEnd = 0;
        beepRepeat = 0;
        delay(30);
        startBeep(120, 2);
        break;
      }

      // Chờ beep xong
      if (millis() < beepEnd || beepRepeat > 0) break;

      // Beep xong → kiểm tra session
      {
        SessionStatus sStatus = getSessionStatus();
        if (sStatus.locked) {
          state = LOCKED;
          lcdPrint("  HE THONG KHOA ", "  Cho nhan vien..");
          setRGB(1, 0, 0);
          LOG_WARN("System is LOCKED!");
          break;
        }
        if (!sStatus.active) {
          lcdPrint("  Chua bat dau  ", "  phien can!    ");
          delay(1500);
          stableCount = 0;
          stableBeepDone = false;
          lastStable = output;
          state = WEIGHING;
          break;
        }
      }
      state = SENDING;
      break;

    case SENDING:
      {
        String name = "Khong xac dinh";
        float price = 0;
        float wt = 0;

        if (sendToServer(displayed, name, price, wt)) {
          setRGB(0, 1, 0);   // xanh = thành công
          startBeep(80, 3);  // 3 beep ngắn báo có kết quả

          // Dòng 1: tên sản phẩm (tối đa 16 ký tự)
          lcd.clear();
          lcd.setCursor(0, 0);
          if (name.length() > 16) name = name.substring(0, 16);
          lcd.print(name);

          // Dòng 2: giá tiền
          lcd.setCursor(0, 1);
          char buf[17];
          if (price >= 1000) {
            snprintf(buf, sizeof(buf), "%,.0f VND", price);
            // Arduino không hỗ trợ %,  nên format thủ công
            int p = (int)price;
            if (p >= 1000000)
              snprintf(buf, sizeof(buf), "%d.%03d VND", p / 1000000, (p % 1000000) / 1000);
            else if (p >= 1000)
              snprintf(buf, sizeof(buf), "%d.%03d VND", p / 1000, p % 1000);
            else
              snprintf(buf, sizeof(buf), "%d VND", p);
          } else {
            snprintf(buf, sizeof(buf), "%.0f VND", price);
          }
          lcd.print(buf);

          LOG_OK("Display → %s | %s", name.c_str(), buf);

          // Đồng bộ lại rescan_count từ server trước khi chuyển sang trạng thái RESULT
          SessionStatus sStatus = getSessionStatus();
          currentRescanCount = sStatus.rescanCount;
        } else {
          setRGB(1, 0, 0);  // đỏ = lỗi
          startBeep(500, 1);
          lcdPrint("   Loi Server   ", " Kiem tra WiFi..");
          LOG_ERR("sendToServer failed");
          delay(2000);

          // Quay lại WEIGHING để thử lại khi nhấc và đặt lại vật
          stableCount = 0;
          stableBeepDone = false;
          lastStable = displayed;
          state = WEIGHING;
          break;
        }

        state = RESULT;
        break;
      }

    case RESULT:
      if (!usDetect && !weightDetect) {
        ema = displayed = 0;
        stableCount = 0;
        stableBeepDone = false;
        updateIdleDisplay(lastActive);
        state = IDLE;
        LOG_OK("Object removed → IDLE");
        break;
      }

      // Khi vật vẫn ở trên bàn cân, định kỳ kiểm tra trạng thái từ server
      static unsigned long lastResultCheck = 0;
      if (now - lastResultCheck > 1500) {
        lastResultCheck = now;
        SessionStatus sStatus = getSessionStatus();
        if (sStatus.locked) {
          state = LOCKED;
          lcdPrint("  HE THONG KHOA ", "  Cho nhan vien..");
          setRGB(1, 0, 0);
          LOG_WARN("System is LOCKED from RESULT state!");
          break;
        }

        if (sStatus.rescanCount > currentRescanCount) {
          LOG_OK("Rescan triggered! Server count=%d (local=%d)", sStatus.rescanCount, currentRescanCount);
          currentRescanCount = sStatus.rescanCount;

          // Phát 1 beep ngắn
          startBeep(100, 1);

          // Quay về WEIGHING để tự động thực hiện lại quá trình cân và nhận diện
          stableCount = 0;
          stableBeepDone = false;
          lastStable = output;
          state = WEIGHING;
        }
      }
      break;

    case LOCKED:
      // Ở trạng thái LOCKED, bật LED đỏ và hiển thị màn hình khóa
      static unsigned long lastLockCheck = 0;
      if (now - lastLockCheck > 2000) {
        lastLockCheck = now;
        SessionStatus sStatus = getSessionStatus();
        if (!sStatus.locked) {
          LOG_OK("System UNLOCKED by employee!");
          updateIdleDisplay(lastActive);
          state = IDLE;
        }
      }
      break;
  }
}