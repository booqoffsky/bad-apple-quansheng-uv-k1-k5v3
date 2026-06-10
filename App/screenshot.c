/* Copyright 2024 Armel F4HWN
 * https://github.com/armel
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

#include "debugging.h"
#include "driver/st7565.h"
#include "screenshot.h"
#include "misc.h"
#include "driver/vcp.h"
#include "driver/keyboard.h"

// SRAM optimization: minimize static allocations
// - previousHash: one fingerprint per 8-byte chunk instead of a full
//   1024-byte copy of the previous frame (chunks are only compared,
//   never retransmitted from history). A hash collision would skip one
//   stale chunk, which the forcedBlock rotation repairs within at most
//   128 frames anyway.
static uint8_t previousHash[128];
static uint8_t forcedBlock = 0;
static uint8_t keepAlive = 3;

// FNV-1a over one 8-byte chunk, folded to 8 bits
static uint8_t SCREENSHOT_Hash(const uint8_t *data)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < 8; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

void SCREENSHOT_ParseInput(void)
{
    if (SCREENSHOT_IsLocked())
        return;

    if (UART_IsCableConnected()) {
        keepAlive = 15;
        gUSB_ScreenshotEnabled = false;
    }
    else if (VCP_ScreenshotPing()) {
        keepAlive = 15;
        gUSB_ScreenshotEnabled = true;
    }
}

static void SCREENSHOT_Send(const uint8_t *buf, uint16_t len)
{
    if (gUSB_ScreenshotEnabled) {
        cdc_acm_data_send_with_dtr(buf, len);
    } else {
        UART_Send(buf, len);
    }
}

void SCREENSHOT_Line(uint8_t *src, uint8_t *dest, uint16_t *idx) {
    for (uint8_t b = 0; b < 8; b++) {
        for (uint8_t i = 0; i < 128; i += 8) {
            uint8_t acc = 0;
            for (uint8_t k = 0; k < 8; k++) {
                if (src[i + k] & (1 << b)) acc |= (1 << k);
            }
            dest[(*idx)++] = gSetting_set_inv ? ~acc : acc;
        }
    }
}

void SCREENSHOT_Update(bool force)
{
    // Build frame in a temporary stack buffer
    // This is 1024 bytes but it's temporary and gets freed after the function
    uint8_t frameBuffer[1024];
    uint16_t index = 0;
    uint8_t acc = 0;
    uint8_t bitCount = 0;
    static bool wasConnected = false;

    if (SCREENSHOT_IsLocked())
        return;

    if (keepAlive > 0) {
        if (--keepAlive == 0) {
            // Connection just lost → reset state for next reconnection
            wasConnected = false;
            return;
        }
    } else {
        return;
    }

    // Connection is alive — detect reconnection and force full frame
    if (!wasConnected) {
        force = true;
        wasConnected = true;
    }

    // ==== BUILD FRAME ONCE ====
    // Status line: 8 bit layers × 128 columns
    SCREENSHOT_Line(gStatusLine, frameBuffer, &index);

    // Frame buffer: 7 lines × 8 bit layers × 128 columns
    for (uint8_t l = 0; l < 7; l++) {
        SCREENSHOT_Line(gFrameBuffer[l], frameBuffer, &index);
    }

    if (bitCount > 0)
        frameBuffer[index++] = acc;

    if (index != 1024)
        return;

    // ==== FIRST PASS: Count changed chunks ====
    uint16_t deltaLen = 0;
    uint8_t changedChunks[128];  // List of changed chunk indices
    uint8_t newHash[128];        // Fingerprints of the current frame
    uint8_t changedCount = 0;

    for (uint8_t chunk = 0; chunk < 128; chunk++) {
        newHash[chunk] = SCREENSHOT_Hash(&frameBuffer[chunk * 8]);

        bool changed = newHash[chunk] != previousHash[chunk];
        bool isForced = (chunk == forcedBlock);
        bool fullUpdate = force;

        if (changed || isForced || fullUpdate) {
            changedChunks[changedCount++] = chunk;
            deltaLen += 9;
        }
    }

    forcedBlock = (forcedBlock + 1) % 128;

    if (deltaLen == 0)
        return;

    // Skip transmission if a key is currently pressed
    // UART_Send is blocking - would freeze the main loop and lose keypresses
    if (gKeyReading0 != KEY_INVALID)
        return;

    // ==== Send version marker (for backward compatibility detection) ====
    // New format: sends 0xFF before header
    // Old format: doesn't exist, so viewers can differentiate
    uint8_t versionMarker = 0xFF;
    SCREENSHOT_Send(&versionMarker, 1);

    // ==== Send header ====
    uint8_t header[5] = {
        0xAA, 0x55, 0x02,
        (uint8_t)(deltaLen >> 8),
        (uint8_t)(deltaLen & 0xFF)
    };

    SCREENSHOT_Send(header, 5);

    // ==== SECOND PASS: Send only changed chunks ====
    uint8_t chunk[9];
    
    for (uint8_t i = 0; i < changedCount; i++) {
        uint8_t chunkIdx = changedChunks[i];

        chunk[0] = chunkIdx;
        memcpy(&chunk[1], &frameBuffer[chunkIdx * 8], 8);

        SCREENSHOT_Send(chunk, 9);

        // Update the fingerprint only once the chunk is actually sent,
        // so chunks skipped by an early return stay marked as changed
        previousHash[chunkIdx] = newHash[chunkIdx];
    }

    uint8_t end = 0x0A;
    SCREENSHOT_Send(&end, 1);
}