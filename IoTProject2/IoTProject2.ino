#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>

#include "epd_bitmaps.h"
#include "mic_recording.h"

// ===================================================
// DISPLAY
// ===================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void displayText(const String &t) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(t);
  display.display();
}

// ===================================================
// TOUCH
// ===================================================
#define TOUCH_PIN 4
bool touchPreviouslyHigh = false;

// ===================================================
// WIFI
// ===================================================
const char* ssid     = "VNIT_LIB_WIFI";
const char* password = "Lib@54321";

// ===================================================
// GEMINI
// ===================================================
bool geminiMode = false;

// ===================================================
// MIC RECORDING
// ===================================================
bool micRecordingMode = false;
unsigned long micRecordStart = 0;
int I2S_PIN_WS  = 25;  // LRCL
int I2S_PIN_SCK = 26;  // BCLK
int I2S_PIN_SD  = 32;  // DOUT

// ===================================================
// PAGE MODE
// ===================================================
const int MAX_LINES = 40;
const int LINES_PER_PAGE = 7;
String pageLines[MAX_LINES];
int totalLines = 0;
int currentPage = 0;
bool inPageMode = false;

// ===================================================
// ANIMATION
// ===================================================
unsigned long lastFrameTime = 0;
int epdFrame = 0;

// ===================================================
// PAGE TEXT WRAPPING
// ===================================================
void wrapTextIntoLines(const String &text) {
  totalLines = 0;
  currentPage = 0;

  const int textSize = 1;
  const int charWidth = 6 * textSize;
  const int maxCharsPerLine = SCREEN_WIDTH / charWidth;

  String currentLine = "";
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];

    if (c == '\n') {
      if (totalLines < MAX_LINES)
        pageLines[totalLines++] = currentLine;
      currentLine = "";
      continue;
    }

    if ((int)currentLine.length() >= maxCharsPerLine) {
      if (totalLines < MAX_LINES)
        pageLines[totalLines++] = currentLine;
      currentLine = "";
    }

    currentLine += c;
  }

  if (currentLine.length() > 0 && totalLines < MAX_LINES)
    pageLines[totalLines++] = currentLine;
}

void showCurrentPage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int startLine = currentPage * LINES_PER_PAGE;
  int endLine = min(startLine + LINES_PER_PAGE, totalLines);

  int y = 0;
  for (int i = startLine; i < endLine; i++) {
    display.setCursor(0, y);
    display.print(pageLines[i]);
    y += 8;
  }

  display.setCursor(0, 56);
  int totalPages = (totalLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE;

  if (currentPage >= totalPages - 1)
    display.print("Tap to EXIT");
  else
    display.print("Tap for next");

  display.display();
}

void enterPageMode(const String &text) {
  wrapTextIntoLines(text);
  inPageMode = true;
  currentPage = 0;
  showCurrentPage();
}

void handlePageTap() {
  int totalPages = (totalLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE;

  if (currentPage < totalPages - 1) {
    currentPage++;
    showCurrentPage();
  } else {
    inPageMode = false;
    displayText("Ready");
    delay(400);
  }
}

// ===================================================
// WIFI CONNECT
// ===================================================
void connectWiFi() {
  displayText("Connecting WiFi...");
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED)
    displayText("WiFi connected!");
  else {
    displayText("WiFi FAIL!");
    delay(2000);
    ESP.restart();
  }
}

// ===================================================
// FLASK GEMINI TEXT
// ===================================================
String askGemini(const String &question) {

  if (WiFi.status() != WL_CONNECTED)
    return "WiFi disconnected.";

  HTTPClient http;
  http.begin("http://10.107.2.240:5050/gemini");
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"question\":\"" + question + "\"}";
  int httpCode = http.POST(payload);

  if (httpCode != 200)
    return "Server error.";

  String resp = http.getString();

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, resp);

  return doc["reply"].as<String>();
}

// ===================================================
// FLASK GEMINI AUDIO
// ===================================================
String sendWavToGemini(const String &path) {

  if (!SPIFFS.exists(path))
    return "No audio saved.";

  File audioFile = SPIFFS.open(path, "r");
  if (!audioFile)
    return "Open fail";

  HTTPClient http;
  WiFiClient client;

  http.begin(client, "http://10.107.2.240:5050/transcribe");

  String boundary = "----ESP32BOUNDARY";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"record.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  int totalLen = head.length() + audioFile.size() + tail.length();
  http.addHeader("Content-Length", String(totalLen));

  WiFiClient *stream = http.getStreamPtr();

  stream->print(head);

  uint8_t buf[512];
  while (audioFile.available()) {
    int len = audioFile.read(buf, sizeof(buf));
    stream->write(buf, len);
  }

  stream->print(tail);
  audioFile.close();

  int code = http.POST("");
  if (code != 200)
    return "Transcription failed.";

  return http.getString();
}

// ===================================================
// GET QUESTION OVER SERIAL
// ===================================================
String getUserQuestion() {
  displayText("Gemini Mode\nType on Serial...");
  unsigned long start = millis();
  String q = "";

  while (millis() - start < 30000) {
    if (Serial.available()) {
      q = Serial.readStringUntil('\n');
      q.trim();
      if (q.length()) return q;
    }
    delay(50);
  }
  return "";
}

// ===================================================
// ANIMATION
// ===================================================
void playEPDAnimation() {
  if (millis() - lastFrameTime >= 10) {
    display.clearDisplay();
    display.drawXBitmap(0, 0,
                        epd_bitmap_allArray[epdFrame],
                        SCREEN_WIDTH, SCREEN_HEIGHT,
                        SSD1306_WHITE);
    display.display();

    epdFrame = (epdFrame + 1) % epd_bitmap_allArray_LEN;
    lastFrameTime = millis();
  }
}

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  Serial.println("Booting...");

  // --------------------------------------
  // 1. Mount SPIFFS first (before display)
  // --------------------------------------
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS FAIL");
    displayText("SPIFFS FAIL");
    delay(2000);
  } else {
    Serial.println("SPIFFS Mounted");
  }

  // --------------------------------------
  // 2. Init I2C + OLED
  // --------------------------------------
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FAIL");
    while (1) delay(1000);
  }

  displayText("Booting...");
  delay(800);

  // --------------------------------------
  // 3. Microphone INIT
  // --------------------------------------
  initMicrophone();

  // --------------------------------------
  // 4. WiFi
  // --------------------------------------
  connectWiFi();

  // --------------------------------------
  // 5. Ready
  // --------------------------------------
  displayText("Ready.\nTap: Gemini");
}

// ===================================================
// LOOP
// ===================================================
void loop() {

  int tval = touchRead(TOUCH_PIN);
  bool touched = (tval < 800);
  bool rising = (touched && !touchPreviouslyHigh);
  touchPreviouslyHigh = touched;

  // -----------------------
  // PAGE MODE
  // -----------------------
  if (inPageMode) {
    if (rising)
      handlePageTap();
    return;
  }

  // -----------------------
  // GEMINI MODE
  // -----------------------
  if (geminiMode) {

    String q = getUserQuestion();

    if (!q.length()) {
      displayText("Timeout.");
      geminiMode = false;
      return;
    }

    displayText("Thinking...");
    String ans = askGemini(q);

    enterPageMode("AI: " + ans);
    geminiMode = false;
    return;
  }

  // -----------------------
  // MIC RECORDING MODE
  // -----------------------
  if (!geminiMode && rising && !micRecordingMode) {
    micRecordingMode = true;
    displayText("Recording...");
    startMicRecording();
    micRecordStart = millis();
    return;
  }

  if (micRecordingMode) {

    recordMicChunk();   // 🔥 Continuous audio recording

    // Tap to stop
    if (rising) {
      stopMicRecording();
      displayText("Saved! Sending...");
      delay(500);

      micRecordingMode = false;

      String transcript = sendWavToGemini("/record.wav");

      enterPageMode("AI: " + transcript);
      return;
    }

    // Auto-stop after 5 seconds
    if (millis() - micRecordStart > 5000) {
      stopMicRecording();
      displayText("Saved! Sending...");
      delay(500);

      micRecordingMode = false;

      String transcript = sendWavToGemini("/record.wav");

      enterPageMode("AI: " + transcript);
      return;
    }

    return;
  }

  // -----------------------
  // ENTER GEMINI MODE
  // -----------------------
  if (rising) {
    geminiMode = true;
    displayText("Gemini Mode");
    delay(300);
    return;
  }

  // -----------------------
  // IDLE → animation
  // -----------------------
  playEPDAnimation();
}