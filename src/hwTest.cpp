#ifndef RUN_PAPER_TEST
#ifdef RUN_HW_TEST
#include <RadioLib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "EspHal.hpp"
#include "LoRaProtocol.hpp"
#include "Ssd1306.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LmpHWTest";

// Global Hardware Instances
EspHal hal_inst(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);
EspHal *hal = &hal_inst;
Module radioModule_inst(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);
SX1262 radio(&radioModule_inst);
LoRaProtocol protocol_inst(&radio, hal, HELTEC_LORA_DIO1);
LoRaProtocol *protocol = &protocol_inst;
Ssd1306 oled;

// Shared state for logging
static esp_err_t oledErr = ESP_FAIL;
static int testCaseCount = 0;
static uint32_t rxSuccessCount = 0;
static float lastRssi = 0.0f;
static float lastSnr = 0.0f;
static std::string lastStatusStr = "Ready";

void updateOledDisplay(const std::string &header, const std::string &line1, const std::string &line2, const std::string &line3)
{
  if (oledErr != ESP_OK)
    return;
  oled.clear();
  oled.print(0, 0, header.c_str());
  oled.print(2, 0, line1.c_str());
  oled.print(4, 0, line2.c_str());
  oled.print(6, 0, line3.c_str());
  oled.update();
}

// MAC/PHY HAL-Level CAD sensing helper function
bool performCadSensing(SX1262 *radioPtr, EspHal *halPtr, int maxRetries = 5, uint32_t baseBackoffMs = 20)
{
  int attempts = 0;
  while (attempts < maxRetries)
  {
    int cadState = radioPtr->scanChannel();
    if (cadState == RADIOLIB_PREAMBLE_DETECTED)
    {
      uint32_t backoffMs = (rand() % (1 << attempts)) * baseBackoffMs + 10;
      ESP_LOGW(TAG, "[MAC/PHY CAD] Active RF preamble detected! Backing off %ums (attempt %d/%d)",
               (unsigned)backoffMs, attempts + 1, maxRetries);
      halPtr->delay(backoffMs);
      attempts++;
    }
    else
    {
      if (attempts > 0)
      {
	ESP_LOGI(TAG, "[MAC/PHY CAD] Channel clear after %d backoff attempts. Proceeding with TX.", attempts);
      }
      return true;
    }
  }
  ESP_LOGW(TAG, "[MAC/PHY CAD] Channel busy after %d attempts. Transmitting anyway (best-effort).", maxRetries);
  return false;
}

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "=====================================================");
  ESP_LOGI(TAG, "=== LMP v2 4-Mode & Address Validation Test Active ==");
  ESP_LOGI(TAG, "=====================================================");

  // Initialize Vext Power
  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0);  // Active LOW
  vTaskDelay(pdMS_TO_TICKS(100));

  // Initialize OLED Display
  oledErr = oled.init();
  updateOledDisplay("LMP v2 HW TEST", "Initializing...", "", "");

  // Initialize HAL & RadioLib
  hal->init();
  int state = radio.begin(868.0);
  if (state != RADIOLIB_ERR_NONE)
  {
    ESP_LOGE(TAG, "Radio Init Failed: %d", state);
    updateOledDisplay("LMP v2 HW TEST", "Radio FAIL!", "", "");
    while (1)
      vTaskDelay(1000);
  }

  // Radio Configurations
  radio.setSpreadingFactor(7);
  radio.setBandwidth(500.0);
  radio.setCodingRate(5);
  radio.setSyncWord(0x12);
  radio.setOutputPower(15);  // moderate power for indoor testing
  radio.setPreambleLength(8);

  // Initialize protocol stack
  protocol->setYieldCallback([]()
                             { vTaskDelay(1); });
  protocol->setVerbose(true);

#ifdef NODE_MODE_RX
  uint8_t myAddr = 0x02;
  protocol->setNodeAddress(myAddr);
  ESP_LOGI(TAG, "Running in RECEIVER Mode (My Node Addr: 0x%02X).", myAddr);

  updateOledDisplay("LMP RX [0x02]", "Waiting Msg...", "RX: 0 | Drop: 0", "RSSI: -- dBm");

  protocol->setOnConnectionRequestCallback([](SynMetadata &syn) -> bool
                                           {
    ESP_LOGI(TAG, "[RX] Accepting 3WHS connection request (reqPayload=%uB)", (unsigned)syn.requestedPayloadSize);
    return true; });

  protocol->setOnReceiveCallback([](const std::vector<uint8_t> &payload, float rssi, float snr)
                                 {
    rxSuccessCount++;
    lastRssi = rssi;
    lastSnr = snr;
    std::string txt(payload.begin(), payload.end());
    ESP_LOGI(TAG, "SUCCESS: >>> MESSAGE DELIVERED! (Size: %u, RSSI=%.1fdBm, SNR=%.1fdB)", 
             (unsigned)payload.size(), rssi, snr);
    ESP_LOGI(TAG, "Content snippet: %s", txt.substr(0, 60).c_str());

    char line1[32], line2[32], line3[32];
    std::snprintf(line1, sizeof(line1), "Delivered: #%u", (unsigned)rxSuccessCount);
    std::snprintf(line2, sizeof(line2), "RX:%u | Drop:%u", (unsigned)rxSuccessCount, (unsigned)protocol->getStats().packetsDroppedAddress);
    std::snprintf(line3, sizeof(line3), "R:%.0fdB S:%.0fdB", rssi, snr);
    updateOledDisplay("LMP RX [0x02]", line1, line2, line3); });

  // Start receive
  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  radio.startReceive();

  while (true)
  {
    uint32_t currentMs = pdTICKS_TO_MS(xTaskGetTickCount());
    protocol->update(currentMs);

    // Periodically update OLED status display
    static uint32_t lastOledUpdate = 0;
    if (currentMs - lastOledUpdate > 2000)
    {
      lastOledUpdate = currentMs;
      char line1[32], line2[32], line3[32];
      std::snprintf(line1, sizeof(line1), "State: Listening");
      std::snprintf(line2, sizeof(line2), "RX:%u | Filter:%u", (unsigned)rxSuccessCount, (unsigned)protocol->getStats().packetsDroppedAddress);
      std::snprintf(line3, sizeof(line3), "R:%.0fdB S:%.0fdB", lastRssi, lastSnr);
      updateOledDisplay("LMP RX [0x02]", line1, line2, line3);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }

#else  // NODE_MODE_TX
  uint8_t myAddr = 0x01;
  protocol->setNodeAddress(myAddr);
  ESP_LOGI(TAG, "Running in TRANSMITTER Mode (My Node Addr: 0x%02X).", myAddr);

  updateOledDisplay("LMP TX [0x01]", "Initializing...", "Ready to test", "");
  vTaskDelay(pdMS_TO_TICKS(3000));  // wait for receiver to settle

  struct TestCaseSpec
  {
    const char *name;
    uint8_t targetAddr;
    bool reliable;
    bool stateful;
    size_t payloadLen;
  };

  const TestCaseSpec testCases[] = {
      {"1. Unrel Unicast", 0x02, false, false, 300},
      {"2. Unrel Broadcast", 0xFF, false, false, 200},
      {"3. Filter (Dst 0x05)", 0x05, false, false, 150},
      {"4. Rel Conn-less", 0x02, true, false, 400},
      {"5. Rel Stateful 3WHS", 0x02, true, true, 500},
  };

  size_t numTestCases = sizeof(testCases) / sizeof(testCases[0]);
  size_t currentCaseIdx = 0;

  while (true)
  {
    testCaseCount++;
    const auto &testSpec = testCases[currentCaseIdx];

    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, "TEST CASE %d: Running '%s' (Target: 0x%02X, Size: %uB)",
             testCaseCount, testSpec.name, (unsigned)testSpec.targetAddr, (unsigned)testSpec.payloadLen);

    char l1[32], l2[32], l3[32];
    std::snprintf(l1, sizeof(l1), "Test %d/%u", (unsigned)(currentCaseIdx + 1), (unsigned)numTestCases);
    std::snprintf(l2, sizeof(l2), "%s", testSpec.name);
    std::snprintf(l3, sizeof(l3), "Dst: 0x%02X | Size:%uB", (unsigned)testSpec.targetAddr, (unsigned)testSpec.payloadLen);
    updateOledDisplay("LMP TX [0x01]", l1, l2, l3);

    // Build payload
    std::string testMsg = "TX_NODE_0x01|CASE_" + std::to_string(testCaseCount) + "|" + testSpec.name + "|";
    while (testMsg.size() < testSpec.payloadLen)
    {
      testMsg += "1234567890 ";
    }
    std::vector<uint8_t> txData(testMsg.begin(), testMsg.end());

    uint64_t startMs = hal->millis();
    bool testSuccess = false;

    // MAC/PHY HAL CAD Sensing before transmission
    performCadSensing(&radio, hal);

    if (testSpec.stateful)
    {
      // Stateful test: 3WHS connect, then send, then disconnect
      ESP_LOGI(TAG, "--> Initiating 3WHS connect to 0x%02X...", (unsigned)testSpec.targetAddr);
      if (protocol->connect(testSpec.targetAddr, 5000))
      {
	ESP_LOGI(TAG, "--> 3WHS ESTABLISHED! Sending data...");
	testSuccess = protocol->send(testSpec.targetAddr, txData, testSpec.reliable);
	protocol->disconnect();
      }
      else
      {
	ESP_LOGE(TAG, "--> 3WHS Handshake FAILED!");
	testSuccess = false;
      }
    }
    else
    {
      // Stateless test: direct send
      testSuccess = protocol->send(testSpec.targetAddr, txData, testSpec.reliable);
    }

    uint64_t elapsedMs = hal->millis() - startMs;

    if (testSuccess)
    {
      ESP_LOGI(TAG, "TEST CASE %d RESULT: SUCCESS in %llu ms!", testCaseCount, elapsedMs);
      std::snprintf(l3, sizeof(l3), "PASS! (%llums)", elapsedMs);
    }
    else
    {
      ESP_LOGE(TAG, "TEST CASE %d RESULT: FAILED! in %llu ms", testCaseCount, elapsedMs);
      std::snprintf(l3, sizeof(l3), "FAIL! (%llums)", elapsedMs);
    }
    updateOledDisplay("LMP TX [0x01]", l1, l2, l3);

    // Advance test case index
    currentCaseIdx = (currentCaseIdx + 1) % numTestCases;

    // Pause between test iterations
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

#endif
}
#endif

#endif  // RUN_PAPER_TEST
