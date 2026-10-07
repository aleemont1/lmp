// Hardware bench for the paper: frame-loss emulation, 4 LAMP modes, line-based serial control.
// Driven by simulation/scripts/bench_hw.py. 869.525 MHz, BW 125 kHz, CR 4/5, 8-symbol preamble.
//
// RX commands : "SF <n>", "DROP <percent>"          (drops data/handshake frames, never SACKs, like the simulator's --loss)
// TX commands : "SF <n>", "TRIAL <mode 1-4> <chunks>"
// Output lines (CSV, prefixed): RXF (every frame seen by the RX), RXMSG (message completed), TX (one trial result).
#ifdef RUN_BENCH
#include <RadioLib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "EspHal.hpp"
#include "LoRaProtocol.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "Bench";

EspHal hal_inst(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);
EspHal *hal = &hal_inst;
Module radioModule_inst(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);
SX1262 radio(&radioModule_inst);
LoRaProtocol protocol_inst(&radio, hal, HELTEC_LORA_DIO1);
LoRaProtocol *protocol = &protocol_inst;

static const uint8_t ADDR_TX = 0x01;
static const uint8_t ADDR_RX = 0x02;
static const int MAX_CONNECT_ATTEMPTS = 1 + LoRaMultiPacketConfig::MAX_RETRIES;  // simulator: SYN + R_max resends
static int dropPercent = 0;
static int currentSf = 7;

static bool readLine(std::string &line)
{
  int c;
  while ((c = getchar()) != EOF)
  {
    if (c == '\n' || c == '\r')
    {
      if (!line.empty())
	return true;
      continue;
    }
    line += static_cast<char>(c);
  }
  return false;
}

static void setSf(int sf)
{
  currentSf = sf;
  radio.setSpreadingFactor(sf);
  printf("OK,SF,%d\n", sf);
}

static std::vector<uint8_t> makePayload(size_t size)
{
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i)
    data[i] = static_cast<uint8_t>('A' + (i % 26));
  return data;
}

extern "C" void app_main(void)
{
  esp_log_level_set("*", ESP_LOG_WARN);  // keep the serial line free for CSV

  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0);
  vTaskDelay(pdMS_TO_TICKS(100));

  hal->init();
  int state = radio.begin(869.525);
  if (state != RADIOLIB_ERR_NONE)
  {
    printf("ERR,radio_init,%d\n", state);
    while (1)
      vTaskDelay(1000);
  }
  radio.setSpreadingFactor(7);
  radio.setBandwidth(125.0);
  radio.setCodingRate(5);
  radio.setSyncWord(0x12);
  radio.setOutputPower(10);
  radio.setPreambleLength(8);

  protocol->setYieldCallback([]()
                             { vTaskDelay(1); });
  protocol->setVerbose(false);

#ifdef NODE_MODE_RX
  protocol->setNodeAddress(ADDR_RX);

  protocol->setDropPacketCallback([](const Packet &packet) -> bool
                                  {
    bool drop = false;
    if (!(packet.header.flags & FLAG_ACK) && dropPercent > 0)
      drop = static_cast<int>(esp_random() % 100) < dropPercent;
    printf("RXF,%u,%u,%u,0x%02X,%d,%.1f,%.1f\n", (unsigned)packet.header.messageId, (unsigned)packet.header.chunkIndex,
           (unsigned)packet.header.totalChunks, (unsigned)packet.header.flags, drop ? 1 : 0, radio.getRSSI(), radio.getSNR());
    return drop; });

  protocol->setOnReceiveCallback([](const std::vector<uint8_t> &payload, float rssi, float snr)
                                 { printf("RXMSG,%u,%.1f,%.1f,%lu\n", (unsigned)payload.size(), rssi, snr, (unsigned long)hal->millis()); });

  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  radio.startReceive();
  printf("READY,RX\n");

  std::string line;
  while (true)
  {
    protocol->update(pdTICKS_TO_MS(xTaskGetTickCount()));

    // Non-blocking line assembly: getchar() returns EOF when no byte is pending.
    int c;
    while ((c = getchar()) != EOF)
    {
      if (c != '\n' && c != '\r')
      {
	line += static_cast<char>(c);
	continue;
      }
      int v = 0;
      if (std::sscanf(line.c_str(), "SF %d", &v) == 1)
      {
	setSf(v);
	radio.startReceive();
      }
      else if (std::sscanf(line.c_str(), "DROP %d", &v) == 1)
      {
	dropPercent = v;
	printf("OK,DROP,%d\n", v);
      }
      line.clear();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

#else  // NODE_MODE_TX
  protocol->setNodeAddress(ADDR_TX);
  printf("READY,TX\n");

  std::string line;
  while (true)
  {
    int c = getchar();
    if (c == EOF)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (c != '\n' && c != '\r')
    {
      line += static_cast<char>(c);
      continue;
    }

    int a = 0, b = 0;
    if (std::sscanf(line.c_str(), "SF %d", &a) == 1)
    {
      setSf(a);
    }
    else if (std::sscanf(line.c_str(), "TRIAL %d %d", &a, &b) == 2)
    {
      const int mode = a;
      const bool stateful = (mode == 2 || mode == 4);
      const bool reliable = (mode == 3 || mode == 4);
      const auto before = protocol->getStats();
      const uint32_t startMs = hal->millis();

      bool ok = true;
      if (stateful)
      {
	ok = false;
	for (int attempt = 0; attempt < MAX_CONNECT_ATTEMPTS && !ok; ++attempt)
	  ok = protocol->connect(ADDR_RX);
      }
      if (ok)
	ok = protocol->send(ADDR_RX, makePayload(static_cast<size_t>(b) * LORA_MAX_PAYLOAD_SIZE), reliable);
      if (stateful)
	protocol->disconnect();

      const uint32_t durMs = hal->millis() - startMs;
      const auto after = protocol->getStats();
      printf("TX,%d,%d,%d,%d,%lu,%lu\n", currentSf, mode, b, ok ? 1 : 0, (unsigned long)durMs,
             (unsigned long)(after.chunksTx - before.chunksTx));
    }
    line.clear();
  }
#endif
}
#endif  // RUN_BENCH
