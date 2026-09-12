#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "HWCDC.h"
#include "ESP_I2S.h"
#include "esp_check.h"
#include "es8311.h"
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern HWCDC USBSerial;

#ifndef TCA9554_ADDR
#define TCA9554_ADDR 0x20
#endif

#define EXAMPLE_SAMPLE_RATE 44100
#define EXAMPLE_VOICE_VOLUME 90

struct MODPlayer {
    bool playing;
    const uint8_t* modData;
    size_t modSize;
    uint8_t numChannels;
    uint16_t numPatterns;
    uint8_t songLength;
    uint8_t currentPattern;
    uint8_t currentRow;
    uint16_t speed;
    uint16_t tick;

    struct Sample {
        const uint8_t* data;
        uint32_t length;
        uint32_t repeatStart;
        uint32_t repeatLength;
        uint8_t volume;
        uint8_t finetune;
    } samples[31];

    struct Channel {
        uint8_t sampleNum;
        uint16_t period;
        uint8_t volume;
        float samplePos;
        float sampleInc;
        bool active;
        const uint8_t* sampleData;
        uint32_t sampleLength;
        uint32_t repeatStart;
        uint32_t repeatLength;
        bool looping;
    } channels[4];

    const uint8_t* patternData;
};

void initAudioSystem(void);
void startMODPlayer(void);
void setAudioPlaying(bool play);
void toggleAudioPlayback(void);
bool isAudioPlaying(void);

// ---------------- SONG SWITCHING ----------------
// Swap the currently-playing MOD to a different one from kModSongs[],
// without a reboot. Safe to call from loop() at any time (pauses the MOD
// tick timer for the ~1-2ms the swap takes, so there's no race with the
// timer callback or the core-1 audio task).
void switchToSongIndex(uint8_t index);
void switchToNextSong(void);     // cycles 0 -> 1 -> 2 -> ... -> wraps
void switchToRandomSong(void);   // picks a different random song than the current one
uint8_t getCurrentSongIndex(void);
uint8_t getNumSongs(void);

// Hard stop/resume of the audio engine itself (suspends the I2S/audioTask and
// halts the MOD tick timer), as opposed to setAudioPlaying()/toggleAudioPlayback()
// which just mute the mixer while the task and timer keep running in the
// background. Use stopAudioEngine()/resumeAudioEngine() around CPU/bus-heavy
// operations (tar extraction, firmware updates) where the still-running audio
// task and timer are competing for cycles and causing stutter.
void stopAudioEngine(void);
void resumeAudioEngine(void);
bool isAudioEngineSuspended(void);