#include "audio_engine.h"
#include "esp_system.h"  // for esp_random()

// Each song header (song.h, song2.h, song3.h) defines its MOD data under the
// *same* symbol names (`song[]` / `song_size`), since that's what the .mod
// -> .h converter always emits. To include all three in one translation
// unit without a "redefinition of song" link error, rename the symbols on
// the way in with a macro, then undefine so the next header starts clean.
#define song song1_data
#define song_size song1_data_size
#include "song.h"
#undef song
#undef song_size

#define song song2_data
#define song_size song2_data_size
#include "song2.h"
#undef song
#undef song_size

#define song song3_data
#define song_size song3_data_size
#include "song3.h"
#undef song
#undef song_size

#define song song4_data
#define song_size song4_data_size
#include "song4.h"
#undef song
#undef song_size

#define song song5_data
#define song_size song5_data_size
#include "song5.h"
#undef song
#undef song_size

struct ModSongInfo {
    const uint8_t* data;
    size_t size;
};

static const ModSongInfo kModSongs[] = {
    { song1_data, song1_data_size },
    { song2_data, song2_data_size },
    { song3_data, song3_data_size },
    { song4_data, song4_data_size },
    { song5_data, song5_data_size },
};
#define NUM_MOD_SONGS (sizeof(kModSongs) / sizeof(kModSongs[0]))

// Which song is currently loaded. parseMOD()/processRow() read through
// these instead of a hardcoded array name, so the same code can play
// whichever song was picked at boot.
static const uint8_t* activeSong = nullptr;
static size_t activeSongSize = 0;
static uint8_t currentSongIndex = 0;

static I2SClass i2s;
static es8311_handle_t es_handle = NULL;
static MODPlayer modPlayer;
static esp_timer_handle_t modTimer = NULL;
static volatile bool timerBusy = false;
static const char* TAG = "audio_engine";
static TaskHandle_t audioTaskHandle = NULL;
static volatile bool engineSuspended = false;
static bool wasPlayingBeforeSuspend = false;
static int pendingPatternJump = -1; // set by effect Bxx (position jump), -1 = none
static int pendingRowJump = -1;     // set by effect Dxx (pattern break), -1 = none

// processRow()/playNote() (called from the 20ms MOD tick timer, which runs
// on the ESP-IDF timer service task) and generateMODAudio() (called from
// audioTask, pinned to core 1) both read and write modPlayer.channels[]
// with no prior synchronization. That let the audio task observe a
// half-updated channel mid-note-trigger (e.g. new sampleData with a stale
// sampleLength/repeatStart, or active=true before sampleData was set),
// which drops or corrupts notes - more often on busier, note-dense songs.
// This spinlock makes each side's channel access atomic with respect to
// the other. Nothing under the lock ever blocks (no delay/malloc/etc), so
// contention is bounded to a few microseconds worst case.
static portMUX_TYPE modPlayerMux = portMUX_INITIALIZER_UNLOCKED;

static void IOExpander_Write(uint8_t reg, uint8_t data) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(data);
    uint8_t result = Wire.endTransmission();
    if (result != 0) {
        USBSerial.printf("IOExpander write error: %d (reg 0x%02X)\n", result, reg);
        delayMicroseconds(100);
    }
}

static void IOExpander_Init(void) {
    IOExpander_Write(0x03, 0xFC);
    IOExpander_Write(0x01, 0x00);
    delay(50);
    IOExpander_Write(0x01, 0x03);
    delay(20);
}

static esp_err_t initES8311_codec(void) {
    es_handle = es8311_create(0, ES8311_ADDRESS_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = EXAMPLE_SAMPLE_RATE * 256,
        .sample_frequency = EXAMPLE_SAMPLE_RATE
    };

    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency), TAG, "set sample freq failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set mic failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set volume failed");

    return ESP_OK;
}

static void initI2S(void) {
    i2s.setPins(9, 45, 8, 10, 16);
    if (!i2s.begin(I2S_MODE_STD, EXAMPLE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        USBSerial.println("Failed to initialize I2S bus!");
    }
}

static bool parseMOD(void) {
    modPlayer.modData = activeSong;
    modPlayer.modSize = activeSongSize;

    if (modPlayer.modSize < 1084) return false;

    modPlayer.numChannels = 4;
    modPlayer.songLength = activeSong[950];
    if (modPlayer.songLength > 128) modPlayer.songLength = 128;
    if (modPlayer.songLength == 0) modPlayer.songLength = 1;

    modPlayer.numPatterns = 0;
    for (int i = 0; i < 128; i++) {
        uint8_t pat = activeSong[952 + i];
        if (pat > modPlayer.numPatterns) modPlayer.numPatterns = pat;
    }
    modPlayer.numPatterns++;

    for (int i = 0; i < 31; i++) {
        const uint8_t* s = activeSong + 20 + (i * 30);
        uint16_t length = ((s[22] << 8) | s[23]) << 1;
        uint8_t finetune = s[24] & 0x0F;
        uint8_t volume = s[25];
        uint16_t repeatStart = ((s[26] << 8) | s[27]) << 1;
        uint16_t repeatLen = ((s[28] << 8) | s[29]) << 1;
        if (repeatLen < 4) repeatLen = 0;

        modPlayer.samples[i].length = length;
        modPlayer.samples[i].finetune = finetune;
        modPlayer.samples[i].volume = volume;
        modPlayer.samples[i].repeatStart = repeatStart;
        modPlayer.samples[i].repeatLength = repeatLen;
    }

    uint32_t patternSize = modPlayer.numPatterns * 64 * 4 * modPlayer.numChannels;
    uint32_t sampleOffset = 1084 + patternSize;

    if (sampleOffset >= modPlayer.modSize) return false;

    const uint8_t* ptr = activeSong + sampleOffset;
    for (int i = 0; i < 31; i++) {
        if (modPlayer.samples[i].length > 0) {
            modPlayer.samples[i].data = ptr;
            ptr += modPlayer.samples[i].length;
        }
    }

    modPlayer.patternData = activeSong + 1084;
    return true;
}

static void playNote(int channel, uint8_t sampleNum, uint16_t period) {
    auto& chan = modPlayer.channels[channel];

    if (sampleNum > 0 && sampleNum <= 31) {
        sampleNum--;
        if (modPlayer.samples[sampleNum].length == 0) {
            chan.active = false;
            return;
        }

        chan.sampleNum = sampleNum;
        chan.sampleData = modPlayer.samples[sampleNum].data;
        chan.sampleLength = modPlayer.samples[sampleNum].length;
        chan.repeatStart = modPlayer.samples[sampleNum].repeatStart;
        chan.repeatLength = modPlayer.samples[sampleNum].repeatLength;
        chan.looping = (modPlayer.samples[sampleNum].repeatLength > 0);
        chan.volume = modPlayer.samples[sampleNum].volume;
        chan.active = true;
        // NOTE: no period accompanies this row (instrument-only change) -
        // don't touch samplePos here. Real trackers use this to swap the
        // active instrument/volume without restarting playback; resetting
        // position on every such row causes spurious retriggers/clicks and
        // can cut samples off before they're heard.
    }

    if (period > 0 && period < 2000) {
        chan.period = period;
        chan.sampleInc = 3546895.0f / (period * EXAMPLE_SAMPLE_RATE);
        chan.samplePos = 0; // genuine new note -> (re)start from the top
        if (chan.sampleData != NULL && chan.sampleLength > 0) {
            chan.active = true;
        }
    }
}

static void processRow(void) {
    if (modPlayer.currentPattern >= modPlayer.songLength) {
        modPlayer.currentPattern = 0;
    }

    uint8_t patternIdx = activeSong[952 + modPlayer.currentPattern];
    if (patternIdx >= modPlayer.numPatterns) patternIdx = 0;

    const uint8_t* rowPtr = modPlayer.patternData +
        (patternIdx * 64 * 4 * modPlayer.numChannels) +
        (modPlayer.currentRow * 4 * modPlayer.numChannels);

    pendingPatternJump = -1;
    pendingRowJump = -1;

    portENTER_CRITICAL(&modPlayerMux);
    for (int ch = 0; ch < modPlayer.numChannels; ch++) {
        const uint8_t* d = rowPtr + (ch * 4);
        uint32_t data = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
            ((uint32_t)d[2] << 8) | (uint32_t)d[3];

        uint8_t sampleNum = (d[0] & 0xF0) | (d[2] >> 4);
        uint16_t period = (data >> 16) & 0x0FFF;
        uint8_t effect = d[2] & 0x0F;
        uint8_t param = d[3];

        if (sampleNum > 0 || period > 0) {
            playNote(ch, sampleNum, period);
        }

        switch (effect) {
            case 0x0C: // Set Volume - very commonly used per-note; without
                       // this, any instrument that relies on it to become
                       // audible (rather than its static header volume)
                       // plays silently.
                modPlayer.channels[ch].volume = (param > 64) ? 64 : param;
                break;

            case 0x0B: // Position Jump - jump order table to position 'param'
                pendingPatternJump = param;
                break;

            case 0x0D: // Pattern Break - end current pattern early, continue
                       // at row (param as two BCD digits) of the next pattern
                pendingRowJump = ((param >> 4) * 10) + (param & 0x0F);
                if (pendingRowJump > 63) pendingRowJump = 0;
                break;

            case 0x0F: // Set Speed - only the ticks-per-row form (<32) is
                       // handled here; values >=32 set BPM/tempo, which this
                       // engine doesn't model since its tick timer is fixed.
                if (param > 0 && param < 32) {
                    modPlayer.speed = param;
                }
                break;

            default:
                break;
        }
    }
    portEXIT_CRITICAL(&modPlayerMux);
}

static void generateMODAudio(int16_t* buffer, size_t numSamples) {
    memset(buffer, 0, numSamples * sizeof(int16_t));
    if (!modPlayer.playing) return;

    portENTER_CRITICAL(&modPlayerMux);
    for (size_t i = 0; i < numSamples; i += 2) {
        int32_t mixL = 0, mixR = 0;

        for (int ch = 0; ch < 4; ch++) {
            if (!modPlayer.channels[ch].active) continue;

            auto& chan = modPlayer.channels[ch];
            if (!chan.sampleData || chan.sampleLength == 0) continue;

            uint32_t pos = (uint32_t)chan.samplePos;

            if (pos >= chan.sampleLength) {
                if (chan.looping) {
                    float loopPos = chan.samplePos - chan.sampleLength;
                    while (loopPos >= chan.repeatLength && chan.repeatLength > 0) {
                        loopPos -= chan.repeatLength;
                    }
                    chan.samplePos = chan.repeatStart + loopPos;
                    pos = (uint32_t)chan.samplePos;
                } else {
                    chan.active = false;
                    continue;
                }
            }

            if (pos >= chan.sampleLength) {
                chan.active = false;
                continue;
            }

            int8_t raw = (int8_t)chan.sampleData[pos];
            int16_t sample = (raw * chan.volume) >> 3;

            if (ch == 0 || ch == 3) {
                mixL += sample;
                mixR += sample >> 1;
            } else {
                mixL += sample >> 1;
                mixR += sample;
            }

            chan.samplePos += chan.sampleInc;
        }

        mixL = constrain(mixL, -28000, 28000);
        mixR = constrain(mixR, -28000, 28000);
        buffer[i] = (int16_t)mixL;
        buffer[i + 1] = (int16_t)mixR;
    }
    portEXIT_CRITICAL(&modPlayerMux);
}

static void modTimerCallback(void* arg) {
    if (!modPlayer.playing || timerBusy) return;
    timerBusy = true;

    modPlayer.tick++;
    if (modPlayer.tick >= modPlayer.speed) {
        modPlayer.tick = 0;
        processRow();

        if (pendingPatternJump >= 0 || pendingRowJump >= 0) {
            // Bxx and/or Dxx fired on this row: honor them instead of the
            // normal "next row, or next pattern at row 0" advance.
            if (pendingPatternJump >= 0) {
                modPlayer.currentPattern = pendingPatternJump;
            } else {
                modPlayer.currentPattern++;
            }
            modPlayer.currentRow = (pendingRowJump >= 0) ? pendingRowJump : 0;
            if (modPlayer.currentPattern >= modPlayer.songLength) {
                modPlayer.currentPattern = 0;
            }
        } else {
            modPlayer.currentRow++;
            if (modPlayer.currentRow >= 64) {
                modPlayer.currentRow = 0;
                modPlayer.currentPattern++;
                if (modPlayer.currentPattern >= modPlayer.songLength) {
                    modPlayer.currentPattern = 0;
                }
            }
        }
    }

    timerBusy = false;
}

static void audioTask(void* parameter) {
    const size_t bufferSize = 1024;
    int16_t* audioBuffer = (int16_t*)malloc(bufferSize * sizeof(int16_t));
    if (!audioBuffer) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        generateMODAudio(audioBuffer, bufferSize);
        i2s.write((uint8_t*)audioBuffer, bufferSize * sizeof(int16_t));
        vTaskDelay(1);
    }

    free(audioBuffer);
}

void initAudioSystem(void) {
    IOExpander_Init();
    initI2S();
    initES8311_codec();
}

void startMODPlayer(void) {
    // esp_random() uses the ESP32's hardware RNG, so this is a genuinely
    // different pick each boot (unlike random()/rand() seeded from millis(),
    // which tends to repeat if the board reaches this point in a fairly
    // consistent amount of time after power-on).
    uint32_t songIndex = esp_random() % NUM_MOD_SONGS;
    activeSong = kModSongs[songIndex].data;
    activeSongSize = kModSongs[songIndex].size;
    currentSongIndex = (uint8_t)songIndex;

    if (!parseMOD()) return;

    modPlayer.playing = true;
    modPlayer.speed = 6;
    modPlayer.tick = 0;
    modPlayer.currentPattern = 0;
    modPlayer.currentRow = 0;

    for (int i = 0; i < 4; i++) {
        modPlayer.channels[i].active = false;
        modPlayer.channels[i].sampleData = NULL;
        modPlayer.channels[i].sampleLength = 0;
        modPlayer.channels[i].samplePos = 0;
        modPlayer.channels[i].volume = 0;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &modTimerCallback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mod_timer",
        .skip_unhandled_events = false
    };

    esp_timer_create(&timer_args, &modTimer);
    esp_timer_start_periodic(modTimer, 20000); // 50Hz (20ms)

    // Pin Audio Task to Core 1 so display loops run unhindered on Core 0
    xTaskCreatePinnedToCore(audioTask, "AudioTask", 2048, NULL, 5, &audioTaskHandle, 1);
}

// ---------------- SONG SWITCHING ----------------
// Swaps activeSong/activeSongSize out for a different entry in kModSongs[]
// and re-parses it, without a reboot. Mirrors the sequence startMODPlayer()
// already uses to load the first song, but has to be more careful because
// this time there's a live timer callback and a live audio task that could
// be touching modPlayer at the same instant.
void switchToSongIndex(uint8_t index) {
    if (index >= NUM_MOD_SONGS) return;

    bool wasPlaying = modPlayer.playing;

    // Stop the 20ms MOD tick timer so processRow()/playNote() can't run
    // mid-swap and read half-updated pattern/sample pointers.
    if (modTimer) {
        esp_timer_stop(modTimer); // no-op/harmless if already stopped
    }

    // Silence every channel before touching activeSong. generateMODAudio()
    // (core 1, reads under modPlayerMux) only looks at chan.sampleData/
    // sampleLength/etc while chan.active is true, so clearing 'active' here
    // makes it safe to leave the old song's pointers in channels[] for the
    // brief moment before parseMOD() overwrites them below.
    portENTER_CRITICAL(&modPlayerMux);
    for (int ch = 0; ch < 4; ch++) {
        modPlayer.channels[ch].active = false;
    }
    portEXIT_CRITICAL(&modPlayerMux);

    activeSong = kModSongs[index].data;
    activeSongSize = kModSongs[index].size;
    currentSongIndex = index;

    if (!parseMOD()) {
        // Something's wrong with this song's data - leave audio stopped
        // rather than risk playing garbage pattern/sample data.
        modPlayer.playing = false;
        USBSerial.printf("[Audio] switchToSongIndex(%u) failed to parse\n", index);
        return;
    }

    modPlayer.currentPattern = 0;
    modPlayer.currentRow = 0;
    modPlayer.tick = 0;
    modPlayer.speed = 6;

    for (int ch = 0; ch < 4; ch++) {
        modPlayer.channels[ch].active = false;
        modPlayer.channels[ch].sampleData = NULL;
        modPlayer.channels[ch].sampleLength = 0;
        modPlayer.channels[ch].samplePos = 0;
        modPlayer.channels[ch].volume = 0;
    }

    modPlayer.playing = wasPlaying;

    if (modTimer) {
        esp_timer_start_periodic(modTimer, 20000); // 50Hz (20ms), matches startMODPlayer()
    }

    USBSerial.printf("[Audio] switched to song %u/%u\n", index + 1, (unsigned)NUM_MOD_SONGS);
}

void switchToNextSong(void) {
    switchToSongIndex((currentSongIndex + 1) % NUM_MOD_SONGS);
}

void switchToRandomSong(void) {
    if (NUM_MOD_SONGS <= 1) {
        switchToSongIndex(0);
        return;
    }
    // Re-roll until we land on something other than the current song, so
    // a shake always audibly changes the track instead of sometimes
    // silently re-picking the one that's already playing.
    uint32_t idx;
    do {
        idx = esp_random() % NUM_MOD_SONGS;
    } while (idx == currentSongIndex);
    switchToSongIndex((uint8_t)idx);
}

uint8_t getCurrentSongIndex(void) {
    return currentSongIndex;
}

uint8_t getNumSongs(void) {
    return (uint8_t)NUM_MOD_SONGS;
}

// ---------------- PLAYBACK CONTROL ----------------
// These just gate the mixer/tick callback (modPlayer.playing). The I2S task
// and timer keep running either way (writing silence when paused), so there
// is no start/stop race with the audio task or the 20ms MOD tick timer.
void setAudioPlaying(bool play) {
    modPlayer.playing = play;
}

void toggleAudioPlayback(void) {
    modPlayer.playing = !modPlayer.playing;
    USBSerial.printf("[Audio] %s\n", modPlayer.playing ? "resumed" : "muted");
}

bool isAudioPlaying(void) {
    return modPlayer.playing;
}

// ---------------- HARD ENGINE STOP/RESUME ----------------
// Unlike setAudioPlaying()/toggleAudioPlayback() (which just gate the mixer,
// leaving the I2S write task and the 20ms MOD tick timer running in the
// background), these actually suspend the audio task and stop the timer, so
// they stop competing for CPU/bus time with heavy foreground work like tar
// extraction or OTA flashing. Call resumeAudioEngine() when that work is done.
void stopAudioEngine(void) {
    if (engineSuspended) return;

    wasPlayingBeforeSuspend = modPlayer.playing;
    modPlayer.playing = false;

    if (modTimer) {
        esp_timer_stop(modTimer); // no-op/harmless if already stopped
    }

    if (audioTaskHandle) {
        vTaskSuspend(audioTaskHandle);
    }

    engineSuspended = true;
    USBSerial.println("[Audio] engine suspended (background operation)");
}

void resumeAudioEngine(void) {
    if (!engineSuspended) return;

    if (audioTaskHandle) {
        vTaskResume(audioTaskHandle);
    }

    if (modTimer) {
        esp_timer_start_periodic(modTimer, 20000); // 50Hz (20ms), matches startMODPlayer()
    }

    modPlayer.playing = wasPlayingBeforeSuspend;
    engineSuspended = false;
    USBSerial.println("[Audio] engine resumed");
}

bool isAudioEngineSuspended(void) {
    return engineSuspended;
}