/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <stdbool.h>
#include <string.h>
#include "app/scanner.h"
#include "dcs.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/scanner.h"

void UI_DisplayScanner(void)
{
    char Line1[16];
    char Line2[16];
    char Line3[16];
    const bool searching = (gScanCssState < SCAN_CSS_STATE_FOUND);
    const bool blinkOff  = searching && ((gScanProgressIndicator & 1u) != 0);

    UI_DisplayClear();

    if (gScanSingleFrequency || (gScanCssState != SCAN_CSS_STATE_OFF && gScanCssState != SCAN_CSS_STATE_FAILED)) {
        sprintf(Line2, "Freq:%u.%05u", gScanFrequency / 100000, gScanFrequency % 100000);
    } else {
        strcpy(Line2, "Freq:---.-----");
    }

    if (gScannerSaveState == SCAN_SAVE_CHANNEL) {
        strcpy(Line1, "Save?");
    } else if (gScannerSaveState == SCAN_SAVE_CHAN_SEL) {
        strcpy(Line1, "Save:");
        UI_GenerateChannelStringEx(Line1 + 5, gShowChPrefix, gScanChannel);
    } else if (blinkOff) {
        Line1[0] = '\0';
    } else if (gScanCssState == SCAN_CSS_STATE_OFF) {
        strcpy(Line1, "Search Freq");
    } else if (gScanCssState == SCAN_CSS_STATE_SCANNING) {
        strcpy(Line1, "Search Tone");
    } else if (gScanCssState == SCAN_CSS_STATE_FOUND) {
        strcpy(Line1, "Scan Complete");
    } else {
        strcpy(Line1, "Scan Failed");
    }

    if (gScanCssState < SCAN_CSS_STATE_FOUND) {
        strcpy(Line3, "Tone:---");
    } else if (!gScanUseCssResult) {
        strcpy(Line3, "Tone:None");
    } else if (gScanCssResultType == CODE_TYPE_CONTINUOUS_TONE) {
        sprintf(Line3, "CTCSS:%u.%uHz", CTCSS_Options[gScanCssResultCode] / 10, CTCSS_Options[gScanCssResultCode] % 10);
    } else {
        sprintf(Line3, "DCS:D%03oN", DCS_Options[gScanCssResultCode]);
    }

    UI_PrintString(Line1, 2, 0, 1, 8);
    UI_PrintString(Line2, 2, 0, 3, 8);
    UI_PrintString(Line3, 2, 0, 5, 8);

    ST7565_BlitFullScreen();
}
