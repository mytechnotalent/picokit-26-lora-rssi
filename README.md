![picokit-26-lora-rssi](https://raw.githubusercontent.com/mytechnotalent/picokit-26-lora-rssi/main/picokit-26-lora-rssi.png)

<br>

## FREE Reverse Engineering Self-Study Course [HERE](https://github.com/mytechnotalent/reverse-engineering)
## FREE Embedded Hacking Course [HERE](https://github.com/mytechnotalent/Embedded-Hacking)

<br>

# PICOKIT-26 LORA RSSI

### Signal Quality on the LCD and LEDs and Authenticated Heartbeat
#### Lesson 26 of the Picokit Series

<br>

***
**LEGAL DISCLAIMER:**
The information, tools, and code provided in this repository and course are strictly for educational, research, and defensive purposes only.

You are explicitly prohibited from using any materials contained herein to access, test, modify, or exploit any device, network, or system that you do not own 100% or for which you do not have explicit, documented, and legally binding authorization to interact with.

By using this repository and course, you acknowledge and agree that:

1. Any illegal, unauthorized, or malicious use of this information is solely your responsibility.
2. The author(s) and contributor(s) of this repository and course shall not be held liable for any damages, legal repercussions, criminal charges, or unauthorized actions resulting from the use, misuse, or abuse of the contents herein.
3. You will comply with all applicable local, state, national, and international laws regarding cybersecurity and computer fraud.

**IF YOU DO NOT AGREE WITH THESE TERMS, DO NOT USE THIS REPOSITORY AND COURSE.**
***

<br>
<br>

## Overview

The twenty sixth Picokit lesson. The node samples the DHT11, shows the
temperature and humidity alongside the RSSI and SNR of the last inbound
frame on the 1602 LCD, reflects the signal quality on the status LEDs, and
reports the last RSSI in its authenticated heartbeat.

<br>

## What it teaches

- Reading the RSSI and SNR fields from every inbound +RCV report.
- Rendering the reading and the signal quality on the 1602 I2C LCD.
- Reflecting the signal quality on the red, yellow, and green status LEDs.
- Reporting the last RSSI in the authenticated heartbeat.

<br>

## Hardware

| Peripheral | Pico 2 pin | Role |
| --- | --- | --- |
| Onboard LED | GP25 | heartbeat, one blink per transmit |
| DHT11 | GP4 | temperature and humidity sample |
| 1602 LCD | GP2 SDA / GP3 SCL | reading and signal quality |
| Red LED | GP16 | weak signal |
| Yellow LED | GP18 | fair signal |
| Green LED | GP17 | good signal |
| RYLR998 | GP8 TX / GP9 RX | signal telemetry and heartbeat uplink |
| Debug Probe | SWCLK/SWDIO/GND, GP0/GP1 | SWD and the console |

<br>

## How it works

The node runs `monitor_step` in a loop. Every 2 seconds it samples the
DHT11 and renders `T:23.0C H:61.0%` with `Q:<rssi> S:<snr>` on the LCD.
When a frame arrives the node stores the RSSI and SNR, and lights green for
a strong signal, yellow for a fair signal, or red for a weak signal. Every
5 seconds it seals `{"n":26,"s":<seq>,"q":<rssi>}` with the field key and
sends the heartbeat over LoRa. The gateway authenticates each frame and
only then parses it.

<br>

## Build and flash

```bash
cd firmware
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s
cmake --build build
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "program build/picokit_26_lora_rssi.elf verify reset exit"
```

<br>

## Watch the node

Open the console at 115200 and reset:

```text
BOOT
=== PICOKIT-26 LORA RSSI // SIGNAL QUALITY + AUTHENTICATED HEARTBEAT ===
DHT t=230 h=610
RX from 0x0001 q=-72 s=4
```

<br>

## The gateway

```bash
cd gateway
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 listen.py --port /dev/cu.usbserial-A50285BI --hub 0001 --network 18 --db gateway.db
```

It prints `OK node=26 rssi=... snr=...` per authenticated heartbeat. The
terminal dashboard `python3 tui.py --db gateway.db` and the web dashboard
`python3 web/app.py --db gateway.db` show the same rows.

<br>

## Verify

```bash
python3 .opencode/skill/embedded-c-standard/audit_c_standard.py
python3 .opencode/skill/embedded-python-standard/audit_python_standard.py
python3 .opencode/skill/iot-readme-standard/validate_readme.py
python3 .opencode/skill/iot-banner-standard/validate_banner.py
python3 scripts/run_tests.py
python3 scripts/check_coverage.py
```

<br>

# Next
[picokit-27-lora-store-forward](https://github.com/mytechnotalent/picokit-27-lora-store-forward)

<br>

# License
[MIT License](https://github.com/mytechnotalent/picokit-26-lora-rssi/blob/main/LICENSE)
