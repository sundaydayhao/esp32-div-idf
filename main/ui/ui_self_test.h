#include "c_util.h"
#pragma once

#include "stdint.h"

void AppUiTestHooks_resetToMenu(void);
void AppUiTestHooks_tapMain(int mainIdx);
void AppUiTestHooks_tapSubmenu(const char* groupName, int subIdx);
void AppUiTestHooks_openTopSubmenu(int mainIdx);
void AppUiTestHooks_openOtherLayer(uint8_t layer, int homeIdx,
                                   const char* name);

void UiSelfTest_pollAutoRun(void);
void UiSelfTest_runOnce(void);
void UiSelfTest_armExit(uint32_t ms);
void UiSelfTest_disarmExit(void);
