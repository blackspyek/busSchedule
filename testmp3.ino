// ── Moduł audio ───────────────────────────────────────────────────────────────
// Działa na osobnym FreeRTOS tasku (core 0).
// API: audioSetup() w setup(), audioEnqueue(line) gdy trzeba coś odtworzyć.
// ─────────────────────────────────────────────────────────────────────────────

#include <LittleFS.h>
#include "AudioFileSourceFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

static const float AUDIO_GAIN = 0.12f;

static AudioGeneratorMP3* mp3       = nullptr;
static AudioFileSourceFS* audioFile = nullptr;
static AudioOutputI2S*    out       = nullptr;

// ── Kolejka: FreeRTOS xQueue z char[] — bezpieczna między taskami ────────────
static const int    AUDIO_LINE_LEN = 16; // max długość nazwy linii + '\0'
static QueueHandle_t s_audioQ = nullptr;

// ── Normalizacja nazwy linii → nazwa pliku ────────────────────────────────────
// API zwraca "2", "12" itd., pliki są "autobus002.mp3", "autobus012.mp3"
// Trolejbusy "150"+ już są 3-cyfrowe i nie potrzebują paddingu.
static String normalizeLine(const String& line) {
    bool numeric = !line.isEmpty();
    for (int i = 0; i < (int)line.length(); i++)
        if (!isDigit((unsigned char)line[i])) { numeric = false; break; }
    if (!numeric) return line; // "Biala", "0N1" itp. — bez zmian
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", line.toInt());
    return String(buf);
}

// ── Odtwarzanie ───────────────────────────────────────────────────────────────
static void stopAudio() {
    if (mp3)       { if (mp3->isRunning()) mp3->stop(); delete mp3; mp3 = nullptr; }
    if (audioFile) { delete audioFile; audioFile = nullptr; }
    if (out)       out->SetGain(0.0f); // wycisz wzmacniacz w spoczynku
}

static void startNext(const char* rawLine) {
    String norm = normalizeLine(String(rawLine));
    String path = "/autobus" + norm + ".mp3";

    if (!LittleFS.exists(path)) {
        Serial.println("[Audio] Brak " + path + " -> default");
        path = "/autobusDefault.mp3";
    }
    if (!LittleFS.exists(path)) {
        Serial.println("[Audio] Brak default, pomijam.");
        return;
    }

    if (out) out->SetGain(AUDIO_GAIN); // przywróć głośność przed odtwarzaniem

    audioFile = new AudioFileSourceFS(LittleFS, path.c_str());
    mp3       = new AudioGeneratorMP3();
    if (!mp3->begin(audioFile, out)) {
        Serial.println("[Audio] begin FAIL: " + path);
        stopAudio();
    } else {
        Serial.println("[Audio] Gram: " + path);
    }
}

// ── Task audio (core 0) ───────────────────────────────────────────────────────
static void audioTask(void*) {
    char lineBuf[AUDIO_LINE_LEN];
    for (;;) {
        if (mp3 != nullptr && mp3->isRunning()) {
            if (!mp3->loop()) {
                stopAudio();
                Serial.println("[Audio] Koniec. Sprawdzam kolejke.");
            }
            // Złoty środek: 1 ms usypia task na dokładnie 1 "tik" zegara.
            // To pozwala systemowi pogłaskać Watchdoga, ale jest na tyle szybkie,
            // że bufor przetwornika PCM nie zdąży się opróżnić (nie będzie strzelać).
            vTaskDelay(pdMS_TO_TICKS(1)); 
        } else {
            // Nic nie gra - czekamy max 10ms na nową komendę w kolejce
            if (xQueueReceive(s_audioQ, lineBuf, pdMS_TO_TICKS(10)) == pdTRUE) {
                startNext(lineBuf);
            }
        }
    }
}

// ── API publiczne ─────────────────────────────────────────────────────────────

void audioEnqueue(const String& line) {
    if (!s_audioQ) return;
    char buf[AUDIO_LINE_LEN] = {};
    line.toCharArray(buf, AUDIO_LINE_LEN);
    if (xQueueSend(s_audioQ, buf, 0) != pdTRUE)
        Serial.println("[Audio] Kolejka pelna!");
    else
        Serial.println("[Audio] Kolejka: linia " + line);
}

void audioSetup() {
    if (!LittleFS.begin(true)) {
        Serial.println("[Audio] LittleFS FAIL!"); return;
    }

    s_audioQ = xQueueCreate(10, AUDIO_LINE_LEN);
    if (!s_audioQ) { Serial.println("[Audio] Queue FAIL!"); return; }

    out = new AudioOutputI2S();
    out->SetPinout(1, 2, 42);
    out->SetGain(0.0f); // zacznij wyciszony

    xTaskCreatePinnedToCore(
        audioTask, "audio",
        8192,    // stos — dekoder MP3 potrzebuje miejsca
        nullptr,
        5,       // priorytet wyższy niż loop() (1)
        nullptr,
        0        // core 0
    );

    Serial.println("[Audio] Task na core 0, gain=" + String(AUDIO_GAIN));
}
