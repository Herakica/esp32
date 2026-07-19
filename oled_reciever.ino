#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET = -1;
constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint8_t BUTTON_PIN = 18;
constexpr uint32_t DATA_TIMEOUT_MS = 10000;

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// Must exactly match the sender structure.
struct SensorData {
  float temperature;
  float humidity;
  uint32_t sequence;
};

SensorData latestData;       // Shared only between callback and loop().
SensorData currentData;      // Safe copy used by the display.
SensorData lowestTempData;
SensorData highestTempData;

uint32_t latestDataTime = 0;
uint32_t lastPacketTime = 0;
uint32_t lowestTempTime = 0;
uint32_t highestTempTime = 0;

volatile bool newDataAvailable = false;
portMUX_TYPE dataMutex = portMUX_INITIALIZER_UNLOCKED;

bool hasReceivedData = false;
bool buttonWasPressed = false;
bool noDataScreenShown = false;

void formatElapsedTime(uint32_t milliseconds, char *buffer, size_t bufferSize) {
  const uint32_t totalSeconds = milliseconds / 1000;
  const uint32_t hours = totalSeconds / 3600;
  const uint32_t minutes = (totalSeconds % 3600) / 60;
  const uint32_t seconds = totalSeconds % 60;

  snprintf(buffer, bufferSize, "%02lu:%02lu:%02lu", hours, minutes, seconds);
}

void onDataReceived(
  const esp_now_recv_info_t *info,
  const uint8_t *incomingData,
  int length
) {
  (void)info;

  if (length != sizeof(SensorData)) {
    return;
  }

  portENTER_CRITICAL(&dataMutex);

  memcpy(&latestData, incomingData, sizeof(latestData));
  latestDataTime = millis();
  lastPacketTime = latestDataTime;
  newDataAvailable = true;

  portEXIT_CRITICAL(&dataMutex);
}

void showCurrentData(const SensorData &data) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.printf("T:%.1f C\n", data.temperature);
  display.printf("H:%.1f %%\n", data.humidity);

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.printf("Packet #%lu", data.sequence);

  display.display();
}

void showTemperatureExtremes() {
  char lowTimeText[16];
  char highTimeText[16];

  formatElapsedTime(lowestTempTime, lowTimeText, sizeof(lowTimeText));
  formatElapsedTime(highestTempTime, highTimeText, sizeof(highTimeText));

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("TEMPERATURE RECORDS");

  display.printf("LOW:  %.1f C\n", lowestTempData.temperature);
  display.printf("Time: %s\n", lowTimeText);

  display.printf("HIGH: %.1f C\n", highestTempData.temperature);
  display.printf("Time: %s", highTimeText);

  display.display();
}

void showNoDataScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("NO DATA RECEIVED");
  display.println("Check sender / range");

  display.display();
}

void updateTemperatureRecords(const SensorData &data, uint32_t receivedTime) {
  if (!hasReceivedData) {
    lowestTempData = data;
    highestTempData = data;
    lowestTempTime = receivedTime;
    highestTempTime = receivedTime;
    hasReceivedData = true;
    return;
  }

  if (data.temperature < lowestTempData.temperature) {
    lowestTempData = data;
    lowestTempTime = receivedTime;
  }

  if (data.temperature > highestTempData.temperature) {
    highestTempData = data;
    highestTempTime = receivedTime;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED initialization failed.");

    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Starting ESP-NOW...");
  display.display();

  WiFi.mode(WIFI_STA);

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");

    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onDataReceived);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Waiting for data...");
  display.display();

  Serial.println("Receiver ready.");
}

void loop() {
  SensorData receivedCopy;
  uint32_t receivedTimeCopy = 0;
  uint32_t lastPacketTimeCopy = 0;
  bool gotNewData = false;

  portENTER_CRITICAL(&dataMutex);

  if (newDataAvailable) {
    receivedCopy = latestData;
    receivedTimeCopy = latestDataTime;
    newDataAvailable = false;
    gotNewData = true;
  }

  lastPacketTimeCopy = lastPacketTime;

  portEXIT_CRITICAL(&dataMutex);

  if (gotNewData) {
    currentData = receivedCopy; // Safe display copy; callback cannot alter it.
    updateTemperatureRecords(currentData, receivedTimeCopy);
    noDataScreenShown = false;

    Serial.printf(
      "Received #%lu | T: %.1f C | H: %.1f %%\n",
      currentData.sequence,
      currentData.temperature,
      currentData.humidity
    );
  }

  const bool buttonPressed = digitalRead(BUTTON_PIN) == LOW;

  if (!hasReceivedData) {
    return;
  }

  const bool dataTimedOut =
    millis() - lastPacketTimeCopy > DATA_TIMEOUT_MS;

  if (buttonPressed) {
    // Holding the button always shows stored temperature records.
    if (!buttonWasPressed || gotNewData) {
      showTemperatureExtremes();
    }

    noDataScreenShown = false;
  } else if (dataTimedOut) {
    // Show this once after 10 seconds without a packet.
    if (buttonWasPressed || !noDataScreenShown) {
      showNoDataScreen();
      noDataScreenShown = true;
    }
  } else {
    // Normal display returns immediately when button is released.
    if (buttonWasPressed || gotNewData) {
      showCurrentData(currentData);
    }
  }

  buttonWasPressed = buttonPressed;
}
