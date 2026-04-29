# AutorBus Transit Display

A dual-screen ESP32 transit board that shows live departures for Lublin stops, renders the selected route on a second TFT, and plays MP3 line announcements for departures that are about to leave.

## Why This Project Exists

This project turns an ESP32 with two SPI-connected TFT displays into a small real-time passenger information display:

- the left screen shows upcoming departures for the active stop,
- the right screen shows the route for the selected line,
- a touch-driven modal lets you switch stops directly on the device,
- Wi-Fi onboarding is handled through a captive portal,
- MP3 files stored in LittleFS announce lines that are within the configured alert window.

It is designed like a compact transit dashboard rather than a generic Arduino demo.

## Features

- Live departure refresh from the `zbiorkom.live` API
- Dual-display UI with separate timetable and route views
- Touch-based stop picker with alphabetical jump navigation
- Local stop persistence in LittleFS across restarts
- Route caching to reduce repeated API work
- Background FreeRTOS tasks for departures, route fetches, and audio playback
- MP3 line announcements with fallback audio handling
- Embedded stop catalog, so the device can build the stop picker without downloading metadata first

## Hardware and Software Stack

### Hardware

- ESP32-class board
- 2x TFT displays on shared SPI with independent chip-select lines
- Touch panel supported through `TFT_eSPI`
- I2S audio output for MP3 playback
- Flash layout with a large LittleFS partition for audio assets

### Libraries

- `TFT_eSPI`
- `WiFiManager`
- `ArduinoJson`
- `LittleFS`
- `HTTPClient` / `WiFiClientSecure`
- `ESP8266Audio`-style classes:
  - `AudioGeneratorMP3`
  - `AudioFileSourceFS`
  - `AudioOutputI2S`

## How It Works

### Left Screen: Departures

The main display shows the active stop and the nearest departures returned by the API. The UI keeps a short status line at the bottom with refresh state, last update time, and the current visible range.

### Right Screen: Route View

When you tap a departure card, the device fetches the route for that line and direction, filters the stop list so it starts from the active stop, and renders the result as a compact snake-style path on the second display.

### Touch Stop Selection

The stop picker opens from the main screen and uses an embedded stop list from [`stops_data.h`](stops_data.h). The selected stop is saved to LittleFS so it survives reboots.

### Wi-Fi Onboarding

If the device does not have valid credentials, `WiFiManager` starts an access point named `AutorBus-Setup`. The displays show connection instructions so the device can be configured without reflashing.

### Audio Announcements

The audio subsystem runs on its own FreeRTOS task. When a departure is within the announcement threshold, the sketch queues the line id, resolves it to a file such as `autobus002.mp3`, and plays it through I2S. If a line-specific file is missing, the code falls back to `autobusDefault.mp3`.

## Project Structure

- [`ekrany.ino`](ekrany.ino) contains display rendering, touch handling, Wi-Fi onboarding, API refresh, route loading, and the embedded stop picker flow.
- [`testmp3.ino`](testmp3.ino) contains the dedicated audio queue and MP3 playback task.
- [`stops_data.h`](stops_data.h) embeds the stop id catalog used to build the picker.
- [`partitions.csv`](partitions.csv) reserves a large filesystem partition for audio files.
- [`data/`](data/) stores MP3 assets uploaded to LittleFS.

## Filesystem and Audio Assets

The sketch expects line announcement files inside the LittleFS image:

- numeric lines use zero-padded names such as `autobus002.mp3`, `autobus150.mp3`, or `autobus301.mp3`,
- special lines can use literal names such as `autobusBiala.mp3` or `autobus0N1.mp3`,
- `autobusDefault.mp3` is used as the fallback when no dedicated file exists.

The provided [`partitions.csv`](partitions.csv) allocates:

- `app0` for the firmware image,
- `spiffs` as a large filesystem region for announcement assets.

## Setup

1. Install the required Arduino libraries listed above.
2. Configure `TFT_eSPI` for your display and touch wiring.
3. Build and flash the sketch to your ESP32 board.
4. Upload the contents of [`data/`](data/) to LittleFS.
5. Boot the device and, if needed, connect to the `AutorBus-Setup` access point to provision Wi-Fi.

## API Notes

- Departures come from `https://api.zbiorkom.live/4.8/lublin/stops/getDepartures`
- Route data comes from `https://api.zbiorkom.live/4.8/lublin/routes/getRoute`
- TLS currently uses `client.setInsecure()`, which keeps development simple but skips certificate validation

Because this project depends on live remote data, route/departure availability and exact payload shapes are external runtime dependencies.

## Screenshots

No screenshots are included in the repository yet.

Recommended additions for the GitHub page:

- a photo of the assembled hardware,
- the left departure screen during live operation,
- the right route screen after selecting a line,
- the Wi-Fi onboarding screen.

## Development Notes

- The on-device UI is intentionally kept in Polish.
- Source comments and diagnostics are kept in English for maintainability.
- Stop names and route labels are preserved from the project data and remote API payloads.

## Demo Ideas
