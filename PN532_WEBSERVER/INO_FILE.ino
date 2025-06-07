#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define PN532_IRQ   4
#define PN532_RESET 5 

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

const char* ssid = "ESP32-AP";
const char* password = "12345678";
const char* adminUser = "admin";
const char* adminPass = "admin123";

WebServer server(80);
bool shouldReadCard = false;
String lastScannedUID = "";
bool timeSynced = false;
bool autoAddMode = false;

TaskHandle_t scanTaskHandle = NULL;
volatile bool scanRequest = false;
volatile bool scanInProgress = false;

void scanTask(void *pvParameters) {
  while (1) {
    if (scanRequest && !scanInProgress) {
      scanInProgress = true;
      readRFID();
      scanRequest = false;
      scanInProgress = false;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize PN532
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not found");
  } else {
    nfc.SAMConfig();
    Serial.println("PN532 Ready");
  }

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed");
    while(1);
  }

  // Initialize data files if not exists
  if (!SPIFFS.exists("/database.json")) {
    File file = SPIFFS.open("/database.json", FILE_WRITE);
    if (file) {
      file.print("[]");
      file.close();
    }
  }
  
  if (!SPIFFS.exists("/scan_log.csv")) {
    File file = SPIFFS.open("/scan_log.csv", FILE_WRITE);
    if (file) {
      file.println("Timestamp,UID,Status,Name");
      file.close();
    }
  }

  // Initialize WiFi AP
  WiFi.softAP(ssid, password);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  syncTime();

  // Server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/data", HTTP_GET, handleData);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/style.css", HTTP_GET, handleStaticFile);
  server.on("/dashboard.html", HTTP_GET, handleStaticFile);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/auto-add", HTTP_GET, handleAutoAdd);
  server.on("/auto-add", HTTP_POST, handleAutoAdd);
  server.on("/logs-html", HTTP_GET, handleLogsHtml);
  server.on("/clear-logs", HTTP_POST, handleClearLogs);
  server.begin();
  Serial.println("HTTP server started");

  // Buat task scan di core 0
  xTaskCreatePinnedToCore(
    scanTask,      // Fungsi task
    "ScanTask",   // Nama task
    4096,          // Stack size
    NULL,          // Parameter
    1,             // Prioritas
    &scanTaskHandle, // Handle
    0              // Core 0
  );
}

void loop() {
  // Jalankan webserver di core 1
  server.handleClient();
  readRFID(); // Scan otomatis setiap loop, seperti kode sederhana
  delay(1);
}

// Time functions
void syncTime() {
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Syncing time with NTP...");
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("Failed to sync with NTP, setting manual time");
    setManualTime();
    timeSynced = false;
  } else {
    Serial.println("Time synced with NTP");
    timeSynced = true;
  }
}

void setManualTime() {
  struct tm tm;
  tm.tm_year = 2023 - 1900;
  tm.tm_mon = 0;
  tm.tm_mday = 1;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  time_t t = mktime(&tm);
  struct timeval now = { .tv_sec = t };
  settimeofday(&now, NULL);
}

String getWITATime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01 00:00:00";
  }
  
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

// RFID functions
void readRFID() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;
  
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);
  
  if (success) {
    lastScannedUID = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      lastScannedUID += String(uid[i] < 0x10 ? "0" : "");
      lastScannedUID += String(uid[i], HEX);
    }
    Serial.println("Scanned UID: " + lastScannedUID);
    
    bool isRegistered = checkUIDRegistered(lastScannedUID);
    String userName = getNameFromUID(lastScannedUID);
    
    if (autoAddMode && !isRegistered) {
      if (addUIDToDatabase(lastScannedUID, "Auto-Added")) {
        Serial.println("UID auto-added to database");
        isRegistered = true;
        userName = "Auto-Added";
      }
    }
    
    logScan(lastScannedUID, isRegistered, userName);
    shouldReadCard = false;
    delay(2000); // Tambahkan delay 2 detik antar scan
  }
}

bool addUIDToDatabase(String uid, String name) {
  File file = SPIFFS.open("/database.json", FILE_READ);
  if (!file) return false;

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return false;

  // Check if UID already exists
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (obj["uid"].as<String>() == uid) {
      return false;
    }
  }

  // Add new UID
  JsonObject obj = doc.as<JsonArray>().createNestedObject();
  obj["nama"] = name;
  obj["uid"] = uid;

  file = SPIFFS.open("/database.json", FILE_WRITE);
  if (!file) return false;

  serializeJson(doc, file);
  file.close();
  return true;
}

bool checkUIDRegistered(String uid) {
  File file = SPIFFS.open("/database.json", FILE_READ);
  if (!file) return false;

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return false;

  for (JsonObject obj : doc.as<JsonArray>()) {
    if (obj["uid"].as<String>() == uid) {
      return true;
    }
  }
  return false;
}

String getNameFromUID(String uid) {
  File file = SPIFFS.open("/database.json", FILE_READ);
  if (!file) return "Unknown";

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return "Unknown";

  for (JsonObject obj : doc.as<JsonArray>()) {
    if (obj["uid"].as<String>() == uid) {
      return obj["nama"].as<String>();
    }
  }
  return "Unknown";
}

void logScan(String uid, bool registered, String name) {
  String timestamp = getWITATime();
  String status = registered ? "Registered" : "Unknown";
  String logEntry = timestamp + "," + uid + "," + status + "," + name;
  
  File file = SPIFFS.open("/scan_log.csv", FILE_APPEND);
  if (!file) {
    Serial.println("ERROR: Failed to open scan log");
    return;
  }
  
  file.println(logEntry);
  file.close();
  Serial.println("LOG: " + logEntry);
}

// Web server handlers
void handleStaticFile() {
  String path = server.uri();
  if (path.endsWith(".css")) {
    serveFile(path.c_str(), "text/css");
  } else if (path.endsWith(".html")) {
    serveFile(path.c_str(), "text/html");
  } else {
    server.send(404, "text/plain", "File not found");
  }
}

void serveFile(const char* path, const char* contentType) {
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
  } else {
    server.send(404, "text/plain", "File not found");
  }
}

void handleRoot() {
  if (!SPIFFS.exists("/dashboard.html")) {
    server.send(404, "text/plain", "Dashboard file not found");
    return;
  }

  File file = SPIFFS.open("/dashboard.html", "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open dashboard");
    return;
  }

  server.streamFile(file, "text/html");
  file.close();
}

void handleLogin() {
  if (!server.hasArg("username") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  String username = server.arg("username");
  String pass = server.arg("password");

  if (username == adminUser && pass == adminPass) {
    server.sendHeader("Location", "/dashboard.html");
    server.send(303);
  } else {
    server.send(401, "text/plain", "Unauthorized");
  }
}

void handleAdd() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }

  DynamicJsonDocument newData(256);
  DeserializationError err = deserializeJson(newData, server.arg("plain"));

  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String nama = newData["nama"];
  String uid = newData["uid"].as<String>();

  if (uid.isEmpty()) {
    server.send(400, "text/plain", "UID cannot be empty");
    return;
  }

  if (addUIDToDatabase(uid, nama)) {
    server.send(200, "text/plain", "Data added successfully");
  } else {
    server.send(500, "text/plain", "Failed to add data or UID already exists");
  }
}

void handleData() {
  File file = SPIFFS.open("/database.json", FILE_READ);
  if (!file) {
    server.send(200, "application/json", "[]");
    return;
  }
  
  server.streamFile(file, "application/json");
  file.close();
}

void handleDelete() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "No data received");
    return;
  }

  DynamicJsonDocument input(256);
  DeserializationError error = deserializeJson(input, server.arg("plain"));
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String targetUid = input["uid"];

  File file = SPIFFS.open("/database.json", FILE_READ);
  DynamicJsonDocument doc(1024);
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }

  JsonArray arr = doc.isNull() ? doc.to<JsonArray>() : doc.as<JsonArray>();
  bool found = false;

  for (size_t i = 0; i < arr.size(); i++) {
    if (arr[i]["uid"].as<String>() == targetUid) {
      arr.remove(i);
      found = true;
      break;
    }
  }

  if (!found) {
    server.send(404, "text/plain", "UID not found");
    return;
  }

  file = SPIFFS.open("/database.json", FILE_WRITE);
  if (!file) {
    server.send(500, "text/plain", "Failed to save changes");
    return;
  }

  serializeJson(doc, file);
  file.close();
  server.send(200, "text/plain", "Data deleted successfully");
}

void handleScan() {
  if (scanInProgress) {
    server.send(429, "text/plain", "Scan in progress");
    return;
  }
  scanRequest = true;
  unsigned long startTime = millis();
  while (scanRequest && millis() - startTime < 10000) {
    delay(10);
  }
  if (!lastScannedUID.isEmpty()) {
    server.send(200, "text/plain", lastScannedUID);
    lastScannedUID = "";
  } else {
    server.send(408, "text/plain", "Scan timeout");
  }
}
void handleLogs() {
  if (!SPIFFS.exists("/scan_log.csv")) {
    server.send(404, "text/plain", "No scan logs available");
    return;
  }

  File file = SPIFFS.open("/scan_log.csv", "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open log file");
    return;
  }

  String csv = "";
  while (file.available()) {
    csv += file.readStringUntil('\n') + "\n";
  }
  file.close();

  server.send(200, "text/csv", csv);
}
void handleLogsHtml() {
  if (!SPIFFS.exists("/scan_log.csv")) {
    server.send(404, "text/plain", "No scan logs available");
    return;
  }
  
  File file = SPIFFS.open("/scan_log.csv", "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open log file");
    return;
  }

  // Send as HTML table for better readability
  String html = "<html><head><style>table {border-collapse: collapse;} th, td {border: 1px solid #ddd; padding: 8px;}</style></head><body>";
  html += "<h1>Scan Logs</h1>";
  html += "<table><tr><th>Timestamp</th><th>UID</th><th>Status</th><th>Name</th></tr>";

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.startsWith("Timestamp")) continue;
    int pos1 = line.indexOf(',');
    int pos2 = line.indexOf(',', pos1+1);
    int pos3 = line.indexOf(',', pos2+1);
    if (pos1 > 0 && pos2 > 0 && pos3 > 0) {
      String timestamp = line.substring(0, pos1);
      String uid = line.substring(pos1+1, pos2);
      String status = line.substring(pos2+1, pos3);
      String name = line.substring(pos3+1);
      html += "<tr><td>" + timestamp + "</td><td>" + uid + "</td><td>" + status + "</td><td>" + name + "</td></tr>";
    }
  }
  
  html += "</table></body></html>";
  file.close();
  
  server.send(200, "text/html", html);
}

void handleAutoAdd() {
  if (!server.hasArg("enable")) {
    server.send(400, "text/plain", "Missing 'enable' parameter");
    return;
  }

  autoAddMode = server.arg("enable") == "true";
  server.send(200, "text/plain", autoAddMode ? "Auto-add enabled" : "Auto-add disabled");
}

void handleClearLogs() {
  if (SPIFFS.exists("/scan_log.csv")) {
    SPIFFS.remove("/scan_log.csv");
  }
  // Buat ulang file log dengan header
  File file = SPIFFS.open("/scan_log.csv", FILE_WRITE);
  if (file) {
    file.println("Timestamp,UID,Status,Name");
    file.close();
    server.send(200, "text/plain", "Logs berhasil dihapus!");
  } else {
    server.send(500, "text/plain", "Gagal menghapus logs!");
  }
}
