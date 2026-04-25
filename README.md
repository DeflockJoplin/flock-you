# Flock-You: Surveillance Device Detector

<img src="flock.png" alt="Flock You" width="300px">

**WiFi wildcard probe request detector (PC dashboard + optional GPS).**

---

## Overview

This fork/build detects devices by **WiFi wildcard probe requests** whose **source MAC OUI** matches the 31-prefix list in [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md). DeFlock Joplin used this data to develop
a tighter filter using raw packet captures from Flock camera sites, and contributed the latest OUI update (`82:6b:f2`). Combining NitekryDPaul's data with this research produced an accurate detection signature. We need more data contributed from real world observations to continue to improve the filter. Please check out the issues page for instructions on how to help with this.

The ESP32 emits **line-delimited JSON over USB serial**, which the PC dashboard in [`api/`](api/) ingests and (optionally) tags with GPS from a USB GPS dongle.

---

## Detection logic

- **Frame type**: 802.11 management **Probe Request**
- **Wildcard SSID**: SSID IE (tag 0) length == 0
- **OUI match**: transmitter/source MAC OUI is in the 31-prefix dataset

---

## Features

- **PC dashboard**: Flask UI at `http://localhost:5000` (see [`api/`](api/))
- **USB serial output**: JSON lines for real-time ingestion
- **Optional GPS**: USB GPS dongle (NMEA) connected to the PC and selected in the web UI

---

## GPS with the PC dashboard

GPS is handled by the PC dashboard in [`api/`](api/). Plug in a USB GPS dongle (NMEA), then in the UI select the GPS serial port and connect.

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | LED (optional) |

---

## Building & Flashing

Firmware uses [PlatformIO Core](https://platformio.org/) (install via Python, not the VS Code extension). A repo-root virtual environment keeps PlatformIO isolated from your system Python and matches the pattern used for the Flask app in `api/`.

```bash
cd flock-you
python3 -m venv .venv
./.venv/bin/pip install --upgrade pip
./.venv/bin/pip install -r requirements.txt


```

Build, flash, and serial monitor (use the `platformio` binary from the same venv):

```bash
./.venv/bin/platformio run                     # build
./.venv/bin/platformio run -t upload         # flash
./.venv/bin/platformio device monitor        # serial output

Windows powershell:
cd flock-you
python -m venv venv
.\venv\Scripts\activate.ps1
pip install -r .\requirements.txt
pio run
pio run -t upload
```

Alternatively, activate the venv once per shell: `source .venv/bin/activate`, then run `platformio …` without the prefix.

This build is intentionally minimal and does not run a phone AP/dashboard.

---

## Flask Companion App

The `api/` folder contains a Flask web application for desktop analysis of detection data.

```bash
cd api
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
./.venv/bin/python flockyou.py
```



Open `http://localhost:5000` for the desktop dashboard.
Live serial ingestion is supported — connect the ESP32 via USB and select the serial port in the Flask UI.

---

## Acknowledgments

- **Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — BLE manufacturer company ID detection (`0x09C8` XUNTONG) sourced from his [flock-you](https://github.com/wgreenberg/flock-you) fork
- **ØяĐöØцяöЪöяцฐ** (@NitekryDPaul) — WiFi OUI research dataset used for probe-request OUI matching (see [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md))
- **[DeFlock](https://deflock.me)** ([FoggedLens/deflock](https://github.com/FoggedLens/deflock)) — crowdsourced ALPR location data and detection methodologies. Datasets included in `datasets/`
- **[GainSec](https://github.com/GainSec)** — Raven BLE service UUID dataset (`raven_configurations.json`) enabling detection of SoundThinking/ShotSpotter acoustic surveillance devices

---

## Original OUI-SPY Firmware Ecosystem

Flock-You is originally part of the OUI-SPY firmware family:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Safety / Raven surveillance detection (this project) | ESP32-S3 |
| **[Sky-Spy](https://github.com/colonelpanichacks/Sky-Spy)** | Drone Remote ID detection | ESP32-S3 / ESP32-C5 |
| **[Remote-ID-Spoofer](https://github.com/colonelpanichacks/Remote-ID-Spoofer)** | WiFi Remote ID spoofer & simulator with swarm mode | ESP32-S3 |
| **[OUI-SPY UniPwn](https://github.com/colonelpanichacks/Oui-Spy-UniPwn)** | Unitree robot exploitation system | ESP32-S3 |

---

## Original Author

**colonelpanichacks**

**Oui-Spy devices available at [colonelpanic.tech](https://colonelpanic.tech)**

---

## Disclaimer

This tool is intended for security research, privacy auditing, and educational purposes. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions. Always comply with local laws regarding wireless scanning and signal interception. The authors are not responsible for misuse.
