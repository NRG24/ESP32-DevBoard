#include "Arduino.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "SD.h"
#include "FS.h"
#include "Audio.h"
#include "ArduinoJson.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SPOTIFY_CLIENT_ID = "YOUR_SPOTIFY_CLIENT_ID";
const char* SPOTIFY_REFRESH_TOKEN = "YOUR_SPOTIFY_REFRESH_TOKEN";

#define SD_CS    5
#define SD_MOSI  23
#define SD_MISO  19
#define SD_SCK   18
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  22
#define BTN_PREV   32
#define BTN_PLAY   33
#define BTN_NEXT   27
#define LED_PIN    2

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_COMMAND_UUID   "12345678-1234-1234-1234-123456789abd"
#define CHAR_STATUS_UUID    "12345678-1234-1234-1234-123456789abe"

Audio audio;

enum PlayMode { MODE_LOCAL, MODE_SPOTIFY };
PlayMode currentMode = MODE_LOCAL;

String tracks[100];
int trackCount = 0;
int currentTrack = 0;
bool isPlaying = false;
int volume = 15;

String spotifyAccessToken = "";
unsigned long tokenExpiry = 0;
String spotifyCurrentTrack = "";
String spotifyCurrentArtist = "";
bool spotifyIsPlaying = false;

unsigned long lastBtnPress = 0;
unsigned long btnPressStart[3] = {0, 0, 0};
bool btnPressed[3] = {false, false, false};
const int DEBOUNCE_MS = 200;
const int LONG_PRESS_MS = 1000;

BLEServer* pServer = NULL;
BLECharacteristic* pCommandChar = NULL;
BLECharacteristic* pStatusChar = NULL;
bool deviceConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    pServer->startAdvertising();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value == "PLAY") {
      if (currentMode == MODE_SPOTIFY) spotifyPlay();
      else localPlay();
    }
    else if (value == "PAUSE") {
      if (currentMode == MODE_SPOTIFY) spotifyPause();
      else localPause();
    }
    else if (value == "NEXT") {
      if (currentMode == MODE_SPOTIFY) spotifyNext();
      else localNext();
    }
    else if (value == "PREV") {
      if (currentMode == MODE_SPOTIFY) spotifyPrev();
      else localPrev();
    }
    else if (value == "MODE_LOCAL") {
      switchMode(MODE_LOCAL);
    }
    else if (value == "MODE_SPOTIFY") {
      switchMode(MODE_SPOTIFY);
    }
    else if (value.startsWith("VOL:")) {
      int newVol = value.substring(4).toInt();
      setVolume(newVol);
    }
    else if (value.startsWith("TOKEN:")) {
      spotifyAccessToken = value.substring(6);
      tokenExpiry = millis() + 3500000;
    }
  }
};

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS)) {
    scanForTracks("/");
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);

  setupBLE();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    wifiAttempts++;
  }

  if (trackCount > 0) {
    localPlayTrack(0);
  }
}

void loop() {
  if (currentMode == MODE_LOCAL) {
    audio.loop();
  }
  handleButtons();

  static unsigned long lastStatusUpdate = 0;
  if (deviceConnected && millis() - lastStatusUpdate > 1000) {
    lastStatusUpdate = millis();
    updateBLEStatus();
  }

  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > (currentMode == MODE_SPOTIFY ? 500 : 1000)) {
    lastBlink = millis();
    if (isPlaying || spotifyIsPlaying) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    } else {
      digitalWrite(LED_PIN, LOW);
    }
  }
}

void setupBLE() {
  BLEDevice::init("ESP32-MP3-Spotify");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCommandChar = pService->createCharacteristic(CHAR_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCommandChar->setCallbacks(new CommandCallbacks());
  pStatusChar = pService->createCharacteristic(CHAR_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void updateBLEStatus() {
  StaticJsonDocument<256> doc;
  doc["mode"] = (currentMode == MODE_SPOTIFY) ? "spotify" : "local";
  doc["playing"] = (currentMode == MODE_SPOTIFY) ? spotifyIsPlaying : isPlaying;
  doc["volume"] = volume;
  if (currentMode == MODE_SPOTIFY) {
    doc["track"] = spotifyCurrentTrack;
    doc["artist"] = spotifyCurrentArtist;
  } else {
    if (currentTrack < trackCount) {
      doc["track"] = tracks[currentTrack];
    }
    doc["trackNum"] = currentTrack + 1;
    doc["totalTracks"] = trackCount;
  }
  String output;
  serializeJson(doc, output);
  pStatusChar->setValue(output.c_str());
  pStatusChar->notify();
}

void handleButtons() {
  unsigned long now = millis();
  bool prevState = digitalRead(BTN_PREV) == LOW;
  bool playState = digitalRead(BTN_PLAY) == LOW;
  bool nextState = digitalRead(BTN_NEXT) == LOW;

  if (prevState && !btnPressed[0]) {
    btnPressed[0] = true;
    btnPressStart[0] = now;
  } else if (!prevState && btnPressed[0]) {
    btnPressed[0] = false;
    if (now - btnPressStart[0] < LONG_PRESS_MS) {
      if (now - lastBtnPress > DEBOUNCE_MS) {
        lastBtnPress = now;
        if (currentMode == MODE_SPOTIFY) spotifyPrev();
        else localPrev();
      }
    } else {
      setVolume(volume - 3);
    }
  }

  if (playState && !btnPressed[1]) {
    btnPressed[1] = true;
    btnPressStart[1] = now;
  } else if (!playState && btnPressed[1]) {
    btnPressed[1] = false;
    if (now - btnPressStart[1] < LONG_PRESS_MS) {
      if (now - lastBtnPress > DEBOUNCE_MS) {
        lastBtnPress = now;
        if (currentMode == MODE_SPOTIFY) {
          if (spotifyIsPlaying) spotifyPause();
          else spotifyPlay();
        } else {
          if (isPlaying) localPause();
          else localPlay();
        }
      }
    } else {
      switchMode(currentMode == MODE_SPOTIFY ? MODE_LOCAL : MODE_SPOTIFY);
    }
  }

  if (nextState && !btnPressed[2]) {
    btnPressed[2] = true;
    btnPressStart[2] = now;
  } else if (!nextState && btnPressed[2]) {
    btnPressed[2] = false;
    if (now - btnPressStart[2] < LONG_PRESS_MS) {
      if (now - lastBtnPress > DEBOUNCE_MS) {
        lastBtnPress = now;
        if (currentMode == MODE_SPOTIFY) spotifyNext();
        else localNext();
      }
    } else {
      setVolume(volume + 3);
    }
  }
}

void switchMode(PlayMode newMode) {
  if (newMode == currentMode) return;
  if (currentMode == MODE_LOCAL) {
    audio.stopSong();
    isPlaying = false;
  }
  currentMode = newMode;
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_PIN, i % 2);
    delay(100);
  }
  if (newMode == MODE_SPOTIFY) {
    fetchSpotifyStatus();
  } else {
    if (trackCount > 0) {
      localPlayTrack(currentTrack);
    }
  }
}

void setVolume(int newVol) {
  volume = constrain(newVol, 0, 21);
  audio.setVolume(volume);
  if (currentMode == MODE_SPOTIFY) {
    spotifySetVolume(volume * 5);
  }
}

void scanForTracks(const char* dirname) {
  File root = SD.open(dirname);
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file && trackCount < 100) {
    if (!file.isDirectory()) {
      String filename = String(file.name());
      if (filename.endsWith(".mp3") || filename.endsWith(".MP3") ||
          filename.endsWith(".Mp3") || filename.endsWith(".mP3")) {
        if (String(dirname) == "/") {
          tracks[trackCount] = "/" + filename;
        } else {
          tracks[trackCount] = String(dirname) + "/" + filename;
        }
        trackCount++;
      }
    } else {
      String subdir;
      if (String(dirname) == "/") {
        subdir = "/" + String(file.name());
      } else {
        subdir = String(dirname) + "/" + String(file.name());
      }
      scanForTracks(subdir.c_str());
    }
    file = root.openNextFile();
  }
}

void localPlayTrack(int index) {
  if (index < 0 || index >= trackCount) return;
  currentTrack = index;
  audio.connecttoFS(SD, tracks[currentTrack].c_str());
  isPlaying = true;
}

void localPlay() {
  if (!isPlaying && currentMode == MODE_LOCAL) {
    if (trackCount > 0) {
      audio.pauseResume();
      isPlaying = true;
    }
  }
}

void localPause() {
  if (isPlaying && currentMode == MODE_LOCAL) {
    audio.pauseResume();
    isPlaying = false;
  }
}

void localNext() {
  currentTrack++;
  if (currentTrack >= trackCount) currentTrack = 0;
  localPlayTrack(currentTrack);
}

void localPrev() {
  currentTrack--;
  if (currentTrack < 0) currentTrack = trackCount - 1;
  localPlayTrack(currentTrack);
}

bool ensureSpotifyToken() {
  if (spotifyAccessToken.length() == 0) return false;
  if (millis() > tokenExpiry) return refreshSpotifyToken();
  return true;
}

bool refreshSpotifyToken() {
  if (WiFi.status() != WL_CONNECTED) return false;
  return false;
}

void spotifyApiCall(const char* method, const char* endpoint, const char* body = nullptr) {
  if (!ensureSpotifyToken()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "https://api.spotify.com/v1" + String(endpoint);
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
  http.addHeader("Content-Type", "application/json");
  int httpCode;
  if (strcmp(method, "GET") == 0) {
    httpCode = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    httpCode = http.PUT(body ? body : "");
  } else if (strcmp(method, "POST") == 0) {
    httpCode = http.POST(body ? body : "");
  } else {
    http.end();
    return;
  }
  if (httpCode == 401) {
    spotifyAccessToken = "";
  }
  http.end();
}

void fetchSpotifyStatus() {
  if (!ensureSpotifyToken()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("https://api.spotify.com/v1/me/player/currently-playing");
  http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      spotifyIsPlaying = doc["is_playing"] | false;
      JsonObject item = doc["item"];
      spotifyCurrentTrack = item["name"].as<String>();
      JsonArray artists = item["artists"];
      if (artists.size() > 0) {
        spotifyCurrentArtist = artists[0]["name"].as<String>();
      }
    }
  } else if (httpCode == 204) {
    spotifyIsPlaying = false;
    spotifyCurrentTrack = "";
    spotifyCurrentArtist = "";
  }
  http.end();
}

void spotifyPlay() {
  spotifyApiCall("PUT", "/me/player/play");
  spotifyIsPlaying = true;
}

void spotifyPause() {
  spotifyApiCall("PUT", "/me/player/pause");
  spotifyIsPlaying = false;
}

void spotifyNext() {
  spotifyApiCall("POST", "/me/player/next");
  delay(300);
  fetchSpotifyStatus();
}

void spotifyPrev() {
  spotifyApiCall("POST", "/me/player/previous");
  delay(300);
  fetchSpotifyStatus();
}

void spotifySetVolume(int volumePercent) {
  volumePercent = constrain(volumePercent, 0, 100);
  String endpoint = "/me/player/volume?volume_percent=" + String(volumePercent);
  spotifyApiCall("PUT", endpoint.c_str());
}

void audio_info(const char *info) {}
void audio_eof_mp3(const char *info) {
  if (currentMode == MODE_LOCAL) localNext();
}
void
