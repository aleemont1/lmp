#include <Arduino.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <esp_now.h>

#include <Packet.hpp>
#include <PacketSerializer.hpp>

// instead of defining RADIO_BOARD_AUTO,
// board can be specified manually
#define RADIO_BOARD_WIFI_LORA32_V3

// now include RadioBoards
// this must be included AFTER RadioLib!
#include <RadioBoards.h>

Radio radio = new RadioModule();

void onDataRecv(const uint8_t *info, const uint8_t *data, int len)
{
  if (len != sizeof(Packet))
  {
    Serial.println("Received malformed packet of size " + String(len));
    return;
  }

  Packet packet;
  memcpy(&packet, data, sizeof(Packet));
  packet.printPacket();
  int state = radio.transmit((uint8_t *)&packet, sizeof(packet));
  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println("LoRa TX complete!");
  }
  else
  {
    Serial.print("LoRa TX failed, code: ");
    Serial.println(state);
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting...");

  // Ensure Wi-Fi is in station mode before initializing ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println("ESP-NOW Receiver Initialized");

  // Inizializza LoRa
  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE)
  {
    Serial.print("Errore radio LoRa: ");
    Serial.println(state);
    return;
  }
  // Imposta i parametri LoRa (opzionale)
  radio.setFrequency(868.0);    // frequenza LoRa EU
  radio.setBandwidth(125.0);    // kHz
  radio.setSpreadingFactor(9);  // 6-12
  radio.setCodingRate(5);       // 5 = 4/5
  radio.setOutputPower(17);     // dBm
  Serial.println("Ricevitore pronto!");
}

void loop()
{
}
