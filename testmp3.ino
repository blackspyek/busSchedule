#include <LittleFS.h>
#include "AudioFileSourceFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

static const float AUDIO_GAIN = 0.12f;
static const int AUDIO_LINE_LEN = 16; // Maximum line name length plus the null terminator.

static AudioGeneratorMP3* mp3 = nullptr;
static AudioFileSourceFS* audioFile = nullptr;
static AudioOutputI2S* out = nullptr;
static QueueHandle_t s_audioQ = nullptr;

// Numeric line ids are stored as zero-padded file names such as autobus002.mp3.
static String normalizeLine(const String& line) {
    bool numeric = !line.isEmpty();
    for (int i = 0; i < static_cast<int>(line.length()); i++) {
        if (!isDigit(static_cast<unsigned char>(line[i]))) {
            numeric = false;
            break;
        }
    }
    if (!numeric) {
        return line;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", line.toInt());
    return String(buf);
}

static void stopAudio() {
    if (mp3) {
        if (mp3->isRunning()) {
            mp3->stop();
        }
        delete mp3;
        mp3 = nullptr;
    }
    if (audioFile) {
        delete audioFile;
        audioFile = nullptr;
    }
    if (out) {
        out->SetGain(0.0f);
    }
}

static void startNext(const char* rawLine) {
    String norm = normalizeLine(String(rawLine));
    String path = "/autobus" + norm + ".mp3";

    if (!LittleFS.exists(path)) {
        Serial.println("[Audio] Missing " + path + " -> falling back to default.");
        path = "/autobusDefault.mp3";
    }
    if (!LittleFS.exists(path)) {
        Serial.println("[Audio] Default track is missing, skipping playback.");
        return;
    }

    if (out) {
        out->SetGain(AUDIO_GAIN);
    }

    audioFile = new AudioFileSourceFS(LittleFS, path.c_str());
    mp3 = new AudioGeneratorMP3();
    if (!mp3->begin(audioFile, out)) {
        Serial.println("[Audio] Failed to start: " + path);
        stopAudio();
    } else {
        Serial.println("[Audio] Playing: " + path);
    }
}

static void audioTask(void*) {
    char lineBuf[AUDIO_LINE_LEN];

    for (;;) {
        if (mp3 != nullptr && mp3->isRunning()) {
            if (!mp3->loop()) {
                stopAudio();
                Serial.println("[Audio] Playback finished, checking the queue.");
            }

            // A 1 ms delay yields often enough to keep the watchdog happy without starving audio.
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (xQueueReceive(s_audioQ, lineBuf, pdMS_TO_TICKS(10)) == pdTRUE) {
            startNext(lineBuf);
        }
    }
}

void audioEnqueue(const String& line) {
    if (!s_audioQ) {
        return;
    }

    char buf[AUDIO_LINE_LEN] = {};
    line.toCharArray(buf, AUDIO_LINE_LEN);
    if (xQueueSend(s_audioQ, buf, 0) != pdTRUE) {
        Serial.println("[Audio] Queue is full.");
    } else {
        Serial.println("[Audio] Queued line " + line);
    }
}

void audioSetup() {
    if (!LittleFS.begin(true)) {
        Serial.println("[Audio] LittleFS mount failed.");
        return;
    }

    s_audioQ = xQueueCreate(10, AUDIO_LINE_LEN);
    if (!s_audioQ) {
        Serial.println("[Audio] Queue allocation failed.");
        return;
    }

    out = new AudioOutputI2S();
    out->SetPinout(1, 2, 42);
    out->SetGain(0.0f);

    xTaskCreatePinnedToCore(
        audioTask, "audio",
        8192, // The MP3 decoder needs a dedicated stack.
        nullptr,
        5,    // Keep audio ahead of loop().
        nullptr,
        0
    );

    Serial.println("[Audio] Task started on core 0, gain=" + String(AUDIO_GAIN));
}
