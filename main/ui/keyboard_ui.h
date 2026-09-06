#include "c_util.h"
#ifndef KEYBOARD_UI_H
#define KEYBOARD_UI_H

#include "text_format.h"

/** Rows in the standard on-screen layout (set cfg.rows = NULL to use it). */
#define OS_KEYBOARD_ROW_COUNT 4

typedef struct OnScreenKeyboardConfig {
  const char* titleLine1;
  const char* titleLine2;

  /** When null, the built-in standard layout (lower / upper / symbols) is used.
   *  ^ toggles uppercase; # toggles the symbol layer. */
  const char* const* rows;
  uint8_t rowCount;

  uint8_t maxLen;

  const char* const* shuffleNames;
  uint8_t shuffleCount;

  int16_t buttonsY;

  const char* backLabel;
  const char* middleLabel;
  const char* okLabel;

  bool enableShuffle;
  bool requireNonEmpty;
  const char* emptyErrorMsg;
} OnScreenKeyboardConfig;
#define OnScreenKeyboardConfig_INITIALIZER {0}
;

typedef struct OnScreenKeyboardResult {
  text_t text;
  bool accepted;
  bool cancelled;
} OnScreenKeyboardResult;
#define OnScreenKeyboardResult_INITIALIZER {0}
;

/** Use the shared standard keyboard; sets rows=NULL and rowCount. */
void osKeyboardUseStandardLayout(OnScreenKeyboardConfig* cfg);

OnScreenKeyboardResult showOnScreenKeyboard(const OnScreenKeyboardConfig* cfg,
                                            text_t initial);

#endif
