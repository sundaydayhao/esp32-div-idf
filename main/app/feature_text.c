#include "feature_text.h"
text_t feature_text_slots[FEATURE_TEXT_SLOTS];
void feature_text_reset(void) {
  memset(feature_text_slots, 0, sizeof(feature_text_slots));
}
