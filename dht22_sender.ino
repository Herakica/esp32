#include <WiFi.h>        // Wi-Fi driver required by ESP-NOW.
#include <esp_now.h>     // ESP-NOW functions.
#include <DHT.h>         // DHT sensor library.

constexpr uint8_t DHT_PIN = 4;           // DHT22 data wire is connected to GPIO 4.
constexpr uint8_t DHT_TYPE = DHT22;      // Select the DHT22 sensor type.
constexpr uint32_t SEND_INTERVAL_MS = 2000; // DHT22 should be read no faster than every 2 seconds.

DHT dht(DHT_PIN, DHT_TYPE); // Create the DHT sensor object.

// This structure must be exactly the same in the receiver sketch.
struct SensorData {
  float temperature;     // Temperature in degrees Celsius.
  float humidity;        // Relative humidity in percent.
  uint32_t sequence;     // Packet number; helps identify fresh packets.
};

SensorData sensorData;   // Stores the next packet to send.
uint32_t lastSendTime = 0; // Time of the last DHT read/transmission.
uint32_t sequenceNumber = 0; // Increases for every successful sensor reading.

// Change this only if the receiver ESP32 has a different MAC address.
const uint8_t receiverMAC[] = {0x70, 0x4B, 0xCA, 0x90, 0xEE, 0xF4};

void setup() {
  Serial.begin(115200); // Start Serial Monitor output.
  dht.begin();          // Start the DHT22 sensor.

  WiFi.mode(WIFI_STA);  // ESP-NOW requires Wi-Fi station mode.

  if (esp_now_init() != ESP_OK) { // Start ESP-NOW.
    Serial.println("ESP-NOW initialization failed.");
    while (true) { delay(1000); } // Stop because communication cannot work.
  }

  esp_now_peer_info_t peerInfo = {};             // Create a zero-initialized peer configuration.
  memcpy(peerInfo.peer_addr, receiverMAC, 6);    // Copy the receiver MAC address.
  peerInfo.channel = 0;                          // Use the current Wi-Fi channel.
  peerInfo.encrypt = false;                      // No encryption; both boards must match.

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {   // Register the receiver as an ESP-NOW peer.
    Serial.println("Could not add receiver peer.");
    while (true) { delay(1000); }                // Stop because packets cannot be sent.
  }

  Serial.print("Sender MAC: ");                  // Useful if you later want receiver-side filtering.
  Serial.println(WiFi.macAddress());
  Serial.println("Sender ready.");
}

void loop() {
  const uint32_t now = millis();                 // Read the current uptime in milliseconds.

  if (now - lastSendTime < SEND_INTERVAL_MS) {   // Wait without blocking the processor.
    return;                                      // Leave loop until two seconds have passed.
  }

  lastSendTime = now;                            // Save the time of this sampling attempt.

  sensorData.temperature = dht.readTemperature(); // Read temperature in Celsius.
  sensorData.humidity = dht.readHumidity();       // Read relative humidity.

  if (isnan(sensorData.temperature) || isnan(sensorData.humidity)) {
    Serial.println("DHT22 read failed.");         // DHT library uses NaN to report an invalid reading.
    return;                                       // Do not transmit bad data.
  }

  sensorData.sequence = ++sequenceNumber;         // Mark this as a new valid packet.

  const esp_err_t result = esp_now_send(          // Queue the packet for ESP-NOW transmission.
    receiverMAC,                                  // Destination MAC address.
    reinterpret_cast<const uint8_t*>(&sensorData),// Treat the structure as raw bytes.
    sizeof(sensorData)                            // Send every byte of the structure.
  );

  if (result == ESP_OK) {                         // ESP_OK means ESP-NOW accepted the packet.
    Serial.printf(
      "Sent #%lu | Temperature: %.1f C | Humidity: %.1f %%\n",
      sensorData.sequence,
      sensorData.temperature,
      sensorData.humidity
    );
  } else {
    Serial.printf("ESP-NOW send error: %s\n", esp_err_to_name(result));
  }
}
