#pragma once

/* Application pages: their drivers and persistence live in components. */
void Terminal_terminalSetup(void);
void Terminal_terminalLoop(void);
void AppSettingsUI_setup(void);
void AppSettingsUI_loop(void);
void SdFileManager_setup(void);
void SdFileManager_loop(void);
void TouchCalib_setup(void);
void TouchCalib_loop(void);
