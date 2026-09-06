#include "c_util.h"
#ifndef UI_UTILS_H
#define UI_UTILS_H

#include "display.h"
#include "network.h"
#include "settings_store.h"
#include "shared.h"
#include "storage_file.h"
#include "touch_driver.h"
#include "touch_ui.h"

extern display_t tft;

// Obfuscated string helpers (XOR decode, then print).
void tftPrintObf(const uint8_t* data, size_t len, uint8_t key);
void tftPrintlnObf(const uint8_t* data, size_t len, uint8_t key);
void serialPrintObf(const uint8_t* data, size_t len, bool newline, uint8_t key);

void updateStatusBar(void);
/** Stop GPS wardriver background task before WiFi/BLE features use the radio.
 */
void pauseBackgroundRadioTasks(void);
float readBatteryVoltage(void);
float readInternalTemperature(void);
bool isSDCardAvailable(void);
/** Recheck SD availability after an external-device session. */
void refreshSdMount(void);
void drawStatusBar(float batteryVoltage, bool forceUpdate,
                   bool bottomSeparator);
/** Request a status bar pass on the next update (e.g. after SD or ward state
 * changes). */
void requestStatusBarRedraw(void);

extern bool feature_exit_requested;

extern void setBrightness(uint8_t value);
bool isButtonPressed(int buttonPin);
/** True while the PCF8574 button for this pin is held (no touch nav). */
bool isPhysicalButtonPressed(int buttonPin);
/** True while a touch nav slot for this pin is held (ignores physical buttons).
 */
bool isTouchNavButtonPressed(int buttonPin);
/** True once per touch nav press (ignores physical buttons). */
bool isTouchNavButtonPressedEdge(int buttonPin);
/** True once per press (touch nav edge or PCF falling edge). */
bool isButtonPressedEdge(int buttonPin);
/** Exit/back: level-sensitive so slow feature loops still catch PCF + touch
 * nav. */
bool featureExitButtonPressed(void);
void setTouchButtonInputEnabled(bool enabled);
void drawTouchButtonCue(void);
void invalidateTouchButtonCue(void);
void redrawTouchButtonBar(void);
/** Clear touch-nav edge state after a full-screen overlay (keyboard, etc.). */
void resetTouchNavHeldState(void);
/** Draw the touch nav bar once (no-op if already drawn). */
void maintainTouchNavBar(void);
/** Pixels reserved at the bottom for the touch nav bar (0 when hidden). */
int16_t touchNavReservedHeight(void);
/** Lowest Y coordinate features should draw into (exclusive of nav bar). */
int16_t touchNavContentBottomY(void);
/** Clear feature body only; leaves the touch nav bar intact. */
void featureClearContent(uint16_t color);
/** True when the on-screen touch nav bar is active for the current feature. */
bool featureHasTouchNavBar(void);
/** Override nav slots with text labels (NULL = default icon for that slot). */
void setTouchNavLabels(const char* left, const char* down, const char* center,
                       const char* up, const char* right);

bool sdMountChipSelect(uint8_t cs);

extern float currentBatteryVoltage;

void initDisplay(void);
void showNotification(const char* title, const char* message);
void hideNotification(void);

typedef uint8_t NotificationAction;
enum {
  NotificationAction_None,
  NotificationAction_Close,
  NotificationAction_Ok,
  NotificationAction_Save
};
void showNotificationActions(const char* title, const char* message,
                             bool showSave);
bool isNotificationVisible(void);
/** Repaint the modal after a full-screen or body redraw covered it (same cached
 * title/message). */
void notificationRedrawIfVisible(void);
NotificationAction notificationHandleTouch(int x, int y);
void printWrappedText(int x, int y, int maxWidth, const char* text);
void loading(int frameDelay, uint16_t color, int16_t x, int16_t y, int repeats,
             bool center);
void displayLogo(uint16_t color, int displayTime);
void initSDCard(void);

typedef uint8_t FeatureUI_ButtonStyle;
enum {
  FeatureUI_ButtonStyle_Primary,
  FeatureUI_ButtonStyle_Secondary,
  FeatureUI_ButtonStyle_Danger
};

typedef struct FeatureUI_Button {
  int16_t x, y, w, h;
  const char* label;
  FeatureUI_ButtonStyle style;
  bool disabled;
} FeatureUI_Button;
#define FeatureUI_Button_INITIALIZER {0}
;

enum { FeatureUI_FOOTER_H = 34 };
enum { FeatureUI_BTN_H = 26 };
enum { FeatureUI_PAD_X = 8 };
enum { FeatureUI_GAP_X = 8 };

void FeatureUI_drawFooterBg(void);

void FeatureUI_drawButtonRect(int x, int y, int w, int h, const char* label,
                              FeatureUI_ButtonStyle style, bool pressed,
                              bool disabled, uint8_t font);

static inline void FeatureUI_drawButton(const FeatureUI_Button* b,
                                        bool pressed) {
  FeatureUI_drawButtonRect((*b).x, (*b).y, (*b).w, (*b).h, (*b).label,
                           (*b).style, pressed, (*b).disabled, 1);
}

void FeatureUI_layoutFooter3(FeatureUI_Button (*btns)[3], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, bool d0, bool d1,
                             bool d2);
void FeatureUI_layoutFooter2(FeatureUI_Button (*btns)[2], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, bool d0, bool d1);
void FeatureUI_layoutFooter4(FeatureUI_Button (*btns)[4], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, const char* l3,
                             FeatureUI_ButtonStyle s3, bool d0, bool d1,
                             bool d2, bool d3);
void FeatureUI_layoutFooter1(FeatureUI_Button* btn, const char* label,
                             FeatureUI_ButtonStyle style, bool disabled);
void FeatureUI_layoutFooter5(FeatureUI_Button (*btns)[5], const char* l0,
                             FeatureUI_ButtonStyle s0, const char* l1,
                             FeatureUI_ButtonStyle s1, const char* l2,
                             FeatureUI_ButtonStyle s2, const char* l3,
                             FeatureUI_ButtonStyle s3, const char* l4,
                             FeatureUI_ButtonStyle s4, bool d0, bool d1,
                             bool d2, bool d3, bool d4);

int FeatureUI_hit(const FeatureUI_Button* btns, int n, int x, int y);

#endif
