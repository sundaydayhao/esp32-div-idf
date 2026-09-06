#include "touch_input.h"

#include "c_vector.h"
#include "esp_log.h"
#include "system_utils.h"
#include "touch_driver.h"

static bool s_touchInitialized;
static const char* TAG = "touch_input";
static uint32_t s_lastTouchDebugMs;
enum {
  kTouchSampleCount = 5,
  kTouchMinValidSamples = 3,
  kTouchMaxStableSpreadRaw = 700
};

esp_err_t touch_input_init(void) {
  if (s_touchInitialized) return ESP_OK;
  esp_err_t error = touch_driver_init();
  s_touchInitialized = error == ESP_OK;
  return error;
}

static bool touchPending(void) { return touch_driver_pending(); }

static bool touch_point_valid(const touch_sample_t* p, uint16_t zThresh) {
  if ((*p).z < (int16_t)zThresh) {
    return false;
  }
  if ((*p).x <= 5 || (*p).x >= 4090 || (*p).y <= 5 || (*p).y >= 4090) {
    return false;
  }
  return true;
}

bool touch_input_read_raw(uint16_t zThresh, int16_t* rawX, int16_t* rawY) {
  if (!s_touchInitialized || !rawX || !rawY) {
    return false;
  }
  if (!touchPending()) {
    return false;
  }

  int16_t xs[kTouchSampleCount] = {0};
  int16_t ys[kTouchSampleCount] = {0};
  int16_t zs[kTouchSampleCount] = {0};
  uint8_t validCount = 0;

  for (uint8_t i = 0; i < kTouchSampleCount; ++i) {
    if (!touchPending()) {
      break;
    }

    touch_sample_t p = touch_driver_sample();
    if (!touch_point_valid(&(p), zThresh)) {
      continue;
    }

    xs[validCount] = p.x;
    ys[validCount] = p.y;
    zs[validCount] = p.z;
    ++validCount;
  }

  if (validCount < kTouchMinValidSamples) {
    return false;
  }

  C_SORT(xs, xs + validCount);
  C_SORT(ys, ys + validCount);
  C_SORT(zs, zs + validCount);

  const uint8_t spreadLo = (validCount >= 4) ? 1 : 0;
  const uint8_t spreadHi =
      (validCount >= 4) ? (uint8_t)(validCount - 2) : (uint8_t)(validCount - 1);
  if ((xs[spreadHi] - xs[spreadLo]) > kTouchMaxStableSpreadRaw ||
      (ys[spreadHi] - ys[spreadLo]) > kTouchMaxStableSpreadRaw) {
    return false;
  }

  (*rawX) = xs[validCount / 2];
  (*rawY) = ys[validCount / 2];
  const uint32_t now = sys_monotonic_ms();
  if (now - s_lastTouchDebugMs >= 250) {
    s_lastTouchDebugMs = now;
    ESP_LOGI(TAG, "raw touch x=%d y=%d z=%d samples=%u irq=%d", (int)(*rawX),
             (int)(*rawY), (int)zs[validCount / 2], (unsigned)validCount,
             (int)touchPending());
  }
  return true;
}
