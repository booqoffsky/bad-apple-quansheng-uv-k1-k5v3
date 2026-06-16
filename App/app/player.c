/* Copyright 2026 booqoffsky
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

#include "app/player.h"

#include <string.h>

#include "app/melody.h"
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/keyboard.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/ui.h"

#define PLAYER_FLASH_BASE 0x012000u
#define PLAYER_MAGIC 0x0BADCAFEu

#define HDR_MAGIC_OFF 0u
#define HDR_VERSION_OFF 4u
#define HDR_COUNT_OFF 8u
#define HDR_OFFSETS_OFF 12u
#define HDR_HEADER_SIZE 12u

#define PLAYER_MAX_RLE_FRAME 2048u

// ==========================================================================
// SPEED CALIBRATION & FINE-TUNING
// Adjust these values if you experience audio/video desynchronization.
// ==========================================================================

// 1. Video frame period in milliseconds.
// - If video is too fast (rushing): increase this value
// - If video is too slow (dragging): decrease this value
// Reference values:
// - 12 FPS: 687u
// - 24 FPS: 275u
#define VIDEO_FRAME_PERIOD_X10MS 275u

// 2. Audio playback speed calibration.
// Uses integer math to avoid floating-point overhead (1000 = 1.0x speed).
// - If audio plays too fast: increase AUDIO_SPEED_MULT (e.g., 1050 for +5% note
// length).
// - If audio plays too slow: decrease AUDIO_SPEED_MULT (e.g., 950 for -5% note
// length).
// Reference values:
// - 12 FPS: 825u
// - 24 FPS: 655u
#define AUDIO_SPEED_MULT 655u
#define AUDIO_SPEED_DIVISOR 1000u

static uint8_t player_frame_buf[LCD_WIDTH * (FRAME_LINES + 1)];
static uint8_t player_rle_buf[PLAYER_MAX_RLE_FRAME];

static uint32_t player_frame_count;

static bool Player_ReadHeader(void) {
    uint8_t hdr[HDR_HEADER_SIZE];
    PY25Q16_ReadBuffer(PLAYER_FLASH_BASE, hdr, sizeof(hdr));

    const uint32_t magic =
        hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != PLAYER_MAGIC) return false;

    player_frame_count =
        hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

    return true;
}

static void Player_GetFrameInfo(uint32_t frame_index, uint32_t* offset, uint16_t* size) {
    const uint32_t off_addr = PLAYER_FLASH_BASE + HDR_OFFSETS_OFF + (frame_index * 4u);
    uint8_t buf[8];
    PY25Q16_ReadBuffer(off_addr, buf, 8);

    const uint32_t off_i =
        buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    const uint32_t off_i1 =
        buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

    *offset = off_i;
    *size = (off_i1 <= off_i) ? 0 : (uint16_t)(off_i1 - off_i);
}

static void Player_DecodeFrame(uint32_t frame_index) {
    uint32_t frame_off;
    uint16_t comp_size;
    Player_GetFrameInfo(frame_index, &frame_off, &comp_size);

    if (comp_size == 0) {
        memset(player_frame_buf, 0, sizeof(player_frame_buf));
        return;
    }

    uint16_t read_size = (comp_size > PLAYER_MAX_RLE_FRAME) ? PLAYER_MAX_RLE_FRAME : comp_size;
    PY25Q16_ReadBuffer(PLAYER_FLASH_BASE + frame_off, player_rle_buf, read_size);

    const uint8_t* src = player_rle_buf;
    const uint8_t* src_end = player_rle_buf + read_size;
    uint8_t* dst = player_frame_buf;
    uint8_t* dst_end = player_frame_buf + sizeof(player_frame_buf);

    // RLE decoding
    while (src < src_end && dst < dst_end) {
        const uint8_t count = *src++;
        if (src >= src_end) break;

        const uint8_t value = *src++;
        uint16_t n = (count == 0) ? 256 : count;  // In RLE, 0 usually means 256
        uint16_t space_left = (uint16_t)(dst_end - dst);
        if (n > space_left) n = space_left;  // Prevent buffer overflow

        memset(dst, value, n);
        dst += n;
    }

    if (dst < dst_end) memset(dst, 0, dst_end - dst);
}

static void Player_BlitFrame(void) {
    memcpy(gStatusLine, &player_frame_buf[0], LCD_WIDTH);
    for (uint8_t p = 0; p < FRAME_LINES; p++)
        memcpy(gFrameBuffer[p], &player_frame_buf[(p + 1) * LCD_WIDTH], LCD_WIDTH);
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

static void Player_ShowNextFrame(uint32_t* frame_index) {
    if (*frame_index + 1u < player_frame_count) {
        *frame_index += 1u;
    }
    Player_DecodeFrame(*frame_index);
    Player_BlitFrame();
}

static uint32_t scale_freq(uint32_t freq) { return ((freq * 103244u + 5000u) / 10000u); }

static void Player_SetToneFreq(const note_t note) {
    BK4819_WriteRegister(BK4819_REG_71, note.melody != 0 ? scale_freq(note.melody) : 0);
    BK4819_WriteRegister(BK4819_REG_72, note.bass != 0 ? scale_freq(note.bass) : 0);
}

static bool Player_PlayNote(const note_t note, uint32_t* frame_index, uint16_t* frame_timer,
                            bool* tx_muted) {
    uint32_t remaining = ((uint32_t)note.duration * AUDIO_SPEED_MULT) / AUDIO_SPEED_DIVISOR;

    if (note.melody == 0 && note.bass == 0) {
        if (!*tx_muted) {
            BK4819_EnterTxMute();
            *tx_muted = true;
        }
    } else {
        Player_SetToneFreq(note);
        if (*tx_muted) {
            BK4819_ExitTxMute();
            *tx_muted = false;
        }
    }

    static uint8_t timing_error_x10 = 0;

    while (remaining > 0) {
        if (KEYBOARD_GetKey() != KEY_INVALID) {
            if (note.melody != 0 || note.bass != 0) {
                BK4819_EnterTxMute();
                *tx_muted = true;
            }
            return false;
        }

        uint32_t timer_x10 = (uint32_t)*frame_timer * 10u + timing_error_x10;
        uint32_t time_to_next_frame_x10 = VIDEO_FRAME_PERIOD_X10MS - timer_x10;

        uint16_t step_ms = (uint16_t)(time_to_next_frame_x10 / 10u);

        if (step_ms == 0 && remaining > 0 && time_to_next_frame_x10 > 0) {
            step_ms = 1u;
        }

        if (step_ms > remaining) {
            step_ms = (uint16_t)remaining;
        }

        SYSTEM_DelayMs(step_ms);

        timer_x10 += (uint32_t)step_ms * 10u;
        remaining -= step_ms;

        if (timer_x10 >= VIDEO_FRAME_PERIOD_X10MS) {
            Player_ShowNextFrame(frame_index);
            timer_x10 -= VIDEO_FRAME_PERIOD_X10MS;
        }

        *frame_timer = (uint16_t)(timer_x10 / 10u);
        timing_error_x10 = (uint8_t)(timer_x10 % 10u);
    }
    return true;
}

static void Player_InitBK4819Audio(void) {
    AUDIO_AudioPathOff();
    BK4819_EnterTxMute();
    BK4819_SetAF(BK4819_AF_BEEP);

    BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 |
                                            (28u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN) |
                                            BK4819_REG_70_ENABLE_TONE2 |
                                            (28u << BK4819_REG_70_SHIFT_TONE2_TUNING_GAIN));

    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, BK4819_REG_30_ENABLE_AF_DAC |
                                            BK4819_REG_30_ENABLE_DISC_MODE |
                                            BK4819_REG_30_ENABLE_TX_DSP);

    SYSTEM_DelayMs(2);
    AUDIO_AudioPathOn();
    SYSTEM_DelayMs(60);
}

static void Player_PlayMelodyWithAnimation(void) {
    const uint16_t saved_reg71 = BK4819_ReadRegister(BK4819_REG_71);
    const uint16_t saved_reg72 = BK4819_ReadRegister(BK4819_REG_72);

    Player_InitBK4819Audio();

    uint32_t frame_index = 0;
    uint16_t frame_timer = 0;
    bool tx_muted = true;

    Player_DecodeFrame(frame_index);
    Player_BlitFrame();

    for (uint16_t i = 0; i < melody_length; i++) {
        if (!Player_PlayNote(melody[i], &frame_index, &frame_timer, &tx_muted)) break;
    }

    if (!tx_muted) {
        BK4819_EnterTxMute();
        tx_muted = true;
    }

    AUDIO_AudioPathOff();
    SYSTEM_DelayMs(5);
    BK4819_TurnsOffTones_TurnsOnRX();
    SYSTEM_DelayMs(5);
    BK4819_WriteRegister(BK4819_REG_71, saved_reg71);
    BK4819_WriteRegister(BK4819_REG_72, saved_reg72);

    if (gEnableSpeaker) AUDIO_AudioPathOn();
}

static void Player_ShowNoFramesMessage(void) {
    UI_DisplayClear();
    UI_PrintString("NO VIDEO DATA", 2, 0, 0, 8);
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
    SYSTEM_DelayMs(3000);
}

void APP_RunVideo(void) {
    BACKLIGHT_UpdateTickless();

    if (!Player_ReadHeader()) {
        Player_ShowNoFramesMessage();
    } else {
        Player_PlayMelodyWithAnimation();
    }

    UI_DisplayClear();
    UI_StatusClear();
    gRequestDisplayScreen = DISPLAY_MAIN;
}
