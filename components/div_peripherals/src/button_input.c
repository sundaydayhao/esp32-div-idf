#include "button_input.h"

#include "board_pins.h"
#include "esp_log.h"
#include "system_utils.h"

static pcf8574_t pcf;
pcf8574_t* button_input_device(void) { return &pcf; }
bool button_input_pressed(int pin) {
  return pin >= 0 && pin < 8 && getPcf8574Address() != 0 &&
         !pcf8574_read_pin(&pcf, pin);
}

#if HAS_PCF8574_BUTTONS
static uint8_t s_pcf8574Addr = 0;
static uint8_t s_pcf8574LastSnapshot = 0xFF;
static uint32_t s_pcf8574LastPollMs = 0;
static uint32_t s_pcf8574PollCount = 0;
static uint32_t s_pcf8574ReadFailCount = 0;
static bool s_pcf8574ScanMode = false;
static uint8_t s_pcf8574ScanLast = 0xFF;
static uint8_t s_pcf8574LastEventSnapshot = 0xFF;
static int s_pcf8574LastEventPin = -1;
static const char* s_pcf8574LastEventReason = NULL;
static uint32_t s_pcf8574LastEventMs = 0;

uint8_t getPcf8574Address(void) { return s_pcf8574Addr; }

static const char* pcf8574ButtonName(int buttonPin) {
  switch (buttonPin) {
    case BTN_UP:
      return "UP";
    case BTN_DOWN:
      return "DOWN";
    case BTN_LEFT:
      return "LEFT";
    case BTN_RIGHT:
      return "RIGHT";
    case BTN_SELECT:
      return "SELECT";
    default:
      return "UNKNOWN";
  }
}

static bool initPcf8574AtAddress(uint8_t address) {
  ESP_LOGI("esp32_div", "PCF8574 probe address 0x%02X", address);
  if (!pcf8574_begin(&(pcf), address)) {
    ESP_LOGW("esp32_div", "PCF8574 probe failed at 0x%02X", address);
    return false;
  }

  s_pcf8574Addr = address;
  ESP_LOGI("esp32_div", "PCF8574 using address 0x%02X", s_pcf8574Addr);

  for (int pin = 0; pin < 8; ++pin) {
    pcf8574_configure_pin(&(pcf), pin, GPIO_MODE_INPUT);
  }
  pcf8574_configure_pin(&(pcf), BTN_UP, GPIO_MODE_INPUT);
  pcf8574_configure_pin(&(pcf), BTN_DOWN, GPIO_MODE_INPUT);
  pcf8574_configure_pin(&(pcf), BTN_LEFT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(&(pcf), BTN_RIGHT, GPIO_MODE_INPUT);
  pcf8574_configure_pin(&(pcf), BTN_SELECT, GPIO_MODE_INPUT);

  for (int pin = 0; pin < 8; pin++) {
    ESP_LOGI("esp32_div", "PCF8574 pin %d = %s", pin,
             pcf8574_read_pin(&(pcf), pin) ? "Released" : "Pressed");
  }
  return true;
}

bool initPcf8574Buttons(void) {
#if PCF8574_ENABLED
  ESP_LOGI("esp32_div", "PCF8574 init starting");
  if (initPcf8574AtAddress(PCF8574_I2C_ADDR)) {
    return true;
  }
  ESP_LOGW("esp32_div",
           "PCF8574 fixed address 0x%02X not responding, scanning 0x20-0x27",
           PCF8574_I2C_ADDR);
  for (uint8_t address = PCF8574_ADDR_MIN; address <= PCF8574_ADDR_MAX;
       ++address) {
    if (address == PCF8574_I2C_ADDR) {
      continue;
    }
    if (initPcf8574AtAddress(address)) {
      return true;
    }
  }
  ESP_LOGW("esp32_div", "PCF8574 no device found on 0x20-0x27");
  return false;
#else
  ESP_LOGI("esp32_div", "PCF8574 disabled by board config");
  s_pcf8574Addr = 0;
  return false;
#endif
}

void logPcf8574ButtonSnapshot(const char* reason, int buttonPin) {
  if (getPcf8574Address() == 0) {
    return;
  }

  uint8_t snapshot = 0xFF;
  if (!pcf8574_read(&(pcf), &(snapshot))) {
    return;
  }

  const uint32_t now = sys_monotonic_ms();
  if (snapshot == s_pcf8574LastEventSnapshot &&
      buttonPin == s_pcf8574LastEventPin &&
      reason == s_pcf8574LastEventReason && now - s_pcf8574LastEventMs < 100) {
    return;
  }
  s_pcf8574LastEventSnapshot = snapshot;
  s_pcf8574LastEventPin = buttonPin;
  s_pcf8574LastEventReason = reason;
  s_pcf8574LastEventMs = now;

  char state[128];
  snprintf(state, sizeof(state),
           "P0=%d P1=%d P2=%d P3=%d P4=%d P5=%d P6=%d P7=%d",
           (snapshot >> 0) & 1, (snapshot >> 1) & 1, (snapshot >> 2) & 1,
           (snapshot >> 3) & 1, (snapshot >> 4) & 1, (snapshot >> 5) & 1,
           (snapshot >> 6) & 1, (snapshot >> 7) & 1);

  ESP_LOGI("esp32_div",
           "PCF8574 self-test reason=%s action=%s pcf_pin=P%d "
           "pressed_mask=0x%02X addr=0x%02X %s",
           reason ? reason : "unknown", pcf8574ButtonName(buttonPin), buttonPin,
           (uint8_t)(~snapshot), getPcf8574Address(), state);
}

static void pcf8574SelfTestTask(void*) {
  uint8_t last = 0xFF;
  while (true) {
    if (getPcf8574Address() != 0) {
      uint8_t snapshot = 0xFF;
      if (pcf8574_read(&(pcf), &(snapshot)) && snapshot != last) {
        last = snapshot;
        char state[128];
        snprintf(state, sizeof(state),
                 "P0=%d P1=%d P2=%d P3=%d P4=%d P5=%d P6=%d P7=%d",
                 (snapshot >> 0) & 1, (snapshot >> 1) & 1, (snapshot >> 2) & 1,
                 (snapshot >> 3) & 1, (snapshot >> 4) & 1, (snapshot >> 5) & 1,
                 (snapshot >> 6) & 1, (snapshot >> 7) & 1);
        ESP_LOGI("esp32_div", "PCF8574 poll addr=0x%02X %s",
                 getPcf8574Address(), state);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void startPcf8574SelfTest(void) {
  s_pcf8574LastPollMs = 0;
  s_pcf8574PollCount = 0;
  s_pcf8574ReadFailCount = 0;
  s_pcf8574ScanMode = false;
  s_pcf8574ScanLast = 0xFF;
  s_pcf8574LastSnapshot = 0xFF;
  s_pcf8574LastEventSnapshot = 0xFF;
  s_pcf8574LastEventPin = -1;
  s_pcf8574LastEventReason = NULL;
  s_pcf8574LastEventMs = 0;
}

void pollPcf8574SelfTest(void) {
  if (getPcf8574Address() == 0) {
    return;
  }

  const uint32_t now = sys_monotonic_ms();
  if (now - s_pcf8574LastPollMs < 200) {
    return;
  }
  s_pcf8574LastPollMs = now;
  ++s_pcf8574PollCount;

  uint8_t snapshot = 0xFF;
  if (!pcf8574_read(&(pcf), &(snapshot))) {
    ++s_pcf8574ReadFailCount;
    ESP_LOGW("esp32_div", "PCF8574 poll failed addr=0x%02X poll=%lu fail=%lu",
             getPcf8574Address(), (unsigned long)s_pcf8574PollCount,
             (unsigned long)s_pcf8574ReadFailCount);
    return;
  }

  if (snapshot == s_pcf8574LastSnapshot) {
    return;
  }
  s_pcf8574LastSnapshot = snapshot;

  char state[128];
  snprintf(state, sizeof(state),
           "P0=%d P1=%d P2=%d P3=%d P4=%d P5=%d P6=%d P7=%d",
           (snapshot >> 0) & 1, (snapshot >> 1) & 1, (snapshot >> 2) & 1,
           (snapshot >> 3) & 1, (snapshot >> 4) & 1, (snapshot >> 5) & 1,
           (snapshot >> 6) & 1, (snapshot >> 7) & 1);
  ESP_LOGI("esp32_div", "PCF8574 poll addr=0x%02X poll=%lu fail=%lu %s",
           getPcf8574Address(), (unsigned long)s_pcf8574PollCount,
           (unsigned long)s_pcf8574ReadFailCount, state);
}

void startPcf8574ScanMode(void) {
  s_pcf8574ScanMode = true;
  s_pcf8574ScanLast = 0xFF;
  ESP_LOGI("esp32_div", "PCF8574 scan mode enabled addr=0x%02X",
           getPcf8574Address());
}

void pollPcf8574ScanMode(void) {
  if (!s_pcf8574ScanMode || getPcf8574Address() == 0) {
    return;
  }

  uint8_t snapshot = 0xFF;
  if (!pcf8574_read(&(pcf), &(snapshot))) {
    ESP_LOGW("esp32_div", "PCF8574 scan read failed addr=0x%02X",
             getPcf8574Address());
    return;
  }

  if (snapshot == s_pcf8574ScanLast) {
    return;
  }
  s_pcf8574ScanLast = snapshot;

  char state[128];
  snprintf(state, sizeof(state),
           "P0=%d P1=%d P2=%d P3=%d P4=%d P5=%d P6=%d P7=%d",
           (snapshot >> 0) & 1, (snapshot >> 1) & 1, (snapshot >> 2) & 1,
           (snapshot >> 3) & 1, (snapshot >> 4) & 1, (snapshot >> 5) & 1,
           (snapshot >> 6) & 1, (snapshot >> 7) & 1);
  ESP_LOGI("esp32_div", "PCF8574 scan addr=0x%02X %s", getPcf8574Address(),
           state);
}
#else
uint8_t getPcf8574Address(void) { return 0; }

bool initPcf8574Buttons(void) { return false; }

void logPcf8574ButtonSnapshot(const char* reason, int buttonPin) {
  (void)reason;
  (void)buttonPin;
}

void startPcf8574SelfTest(void) {}
void pollPcf8574SelfTest(void) {}
void startPcf8574ScanMode(void) {}
void pollPcf8574ScanMode(void) {}
#endif
