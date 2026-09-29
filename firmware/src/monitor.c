// MIT License
//
// Copyright (c) 2026 Kevin Thomas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Author:  Kevin Thomas
// Email:   kevin@mytechnotalent.com
// GitHub:  https://github.com/mytechnotalent/picokit-26-lora-rssi
// File:    monitor.c
// Desc:    Implements the LoRa RSSI state machine that shows the last
//          inbound RSSI and SNR on the LCD and status LEDs.
// Created: 2026

#include "picokit_26_lora_rssi.h"
#include "monitor.h"
#include "radio.h"
#include "status_led.h"
#include "sensor.h"
#include "display.h"
#include "ccm.h"
#include "envelope.h"
#include "field_secrets.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Module-ready flag.
 *
 * Set to true by monitor_init() once the peripherals are configured.
 * monitor_step() returns false while this flag is clear.
 */
static bool g_ready;

/**
 * @brief Initialized I2C peripheral handle for the LCD backpack.
 */
static i2c_inst_t *g_i2c;

/**
 * @brief Initialized I2C backpack address for the LCD.
 */
static uint8_t g_i2c_addr;

/**
 * @brief Monotonic transmit sequence number.
 */
static uint16_t g_seq;

/**
 * @brief Absolute time in microseconds of the next DHT11 sample.
 */
static uint64_t g_next_read_us;

/**
 * @brief Absolute time in microseconds of the next authenticated transmit.
 */
static uint64_t g_next_tx_us;

/**
 * @brief Most recent decoded DHT11 reading.
 */
static dht_reading_t g_reading;

/**
 * @brief RSSI of the last inbound frame in dBm.
 */
static int16_t g_rssi;

/**
 * @brief SNR of the last inbound frame in dB.
 */
static int8_t g_snr;

/**
 * @brief First LCD render line buffer.
 */
static char g_line1[DISPLAY_LINE_LEN];

/**
 * @brief Second LCD render line buffer.
 */
static char g_line2[DISPLAY_LINE_LEN];

/**
 * @brief Inbound radio line accumulator.
 */
static char g_rx_line[RADIO_LINE_BUF_LEN];

/**
 * @brief Number of bytes currently held in the inbound line accumulator.
 */
static size_t g_rx_len;

/**
 * @brief AES-128 session key for telemetry.
 */
static uint8_t g_key[CCM_KEY_LEN];

/**
 * @brief True once the telemetry session key has been loaded.
 */
static bool g_key_ready;

/**
 * @brief Probe one I2C address and report whether it acknowledges.
 *
 * @param i2c Pointer to the I2C peripheral to probe.
 * @param addr The 7-bit address to probe.
 * @return bool true when the address acknowledged.
 */
static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr) {
    uint8_t dummy = 0u;
    if (i2c_write_blocking(i2c, addr, &dummy, 1u, false) < 0) {
        return false;
    }
    printf("  found 0x%02X\n", (unsigned)addr);
    return true;
}

/**
 * @brief Probe the I2C bus and print every device that acknowledges.
 *
 * @param i2c Pointer to the I2C peripheral to scan.
 * @return void
 */
static void i2c_bus_scan(i2c_inst_t *i2c) {
    uint8_t addr;
    uint8_t found = 0u;
    printf("I2C scan:\n");
    for (addr = 0x08u; addr < 0x78u; ++addr) {
        found += i2c_probe(i2c, addr) ? 1u : 0u;
    }
    if (found == 0u) {
        printf("  no devices\n");
    }
}

/**
 * @brief Initialize the I2C bus pins and scan the bus.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_bus_init(void) {
    i2c_init(PICOKIT_26_LORA_RSSI_I2C, PICOKIT_26_LORA_RSSI_I2C_BAUD);
    gpio_set_function(PICOKIT_26_LORA_RSSI_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PICOKIT_26_LORA_RSSI_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PICOKIT_26_LORA_RSSI_I2C_SDA);
    gpio_pull_up(PICOKIT_26_LORA_RSSI_I2C_SCL);
    i2c_bus_scan(PICOKIT_26_LORA_RSSI_I2C);
}

/**
 * @brief Configure the onboard heartbeat LED as a dark output.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init_io(void) {
    gpio_init(PICOKIT_26_LORA_RSSI_LED_PIN);
    gpio_set_dir(PICOKIT_26_LORA_RSSI_LED_PIN, GPIO_OUT);
    gpio_put(PICOKIT_26_LORA_RSSI_LED_PIN, 0);
}

/**
 * @brief Clear the reading, signal, and sequence state.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_reset_signal(void) {
    memset(&g_reading, 0, sizeof(g_reading));
    g_rssi = 0;
    g_snr = 0;
    g_seq = 0u;
}

/**
 * @brief Reset the I2C handle, signal, sequence, and the timing.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init(void) {
    uint64_t now_us = time_us_64();
    g_i2c = PICOKIT_26_LORA_RSSI_I2C;
    g_i2c_addr = PICOKIT_26_LORA_RSSI_LCD_ADDR;
    monitor_reset_signal();
    g_next_read_us = now_us;
    g_next_tx_us = now_us + (uint64_t)PICOKIT_26_LORA_RSSI_TX_INTERVAL_MS * 1000u;
    g_ready = true;
}

/**
 * @brief Load the telemetry session key from the field secret.
 *
 * LAB-ONLY: production must provision the session key through OTP rather
 * than embedding a committed key.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_load_key(void) {
    static const uint8_t key[CCM_KEY_LEN] = FIELD_SECRET_KEY;
    memcpy(g_key, key, CCM_KEY_LEN);
    g_key_ready = true;
}

/**
 * @brief Print the boot banner for the LoRa RSSI lesson.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_banner(void) {
    printf("=== PICOKIT-26 LORA RSSI // SIGNAL QUALITY + AUTHENTICATED HEARTBEAT ===\n");
}

/**
 * @brief Derive the field key and announce a ready monitor.
 *
 * @param void No parameters.
 * @return bool true when the field key was derived and installed.
 */
static bool monitor_finish(void) {
    monitor_load_key();
    monitor_banner();
    return true;
}

/**
 * @brief Blink the onboard heartbeat LED exactly once.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_heartbeat(void) {
    gpio_put(PICOKIT_26_LORA_RSSI_LED_PIN, 1);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
    gpio_put(PICOKIT_26_LORA_RSSI_LED_PIN, 0);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
}

/**
 * @brief Reflect the last inbound signal quality on the status LEDs.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_apply_leds(void) {
    if (g_rssi >= MONITOR_RSSI_GOOD) {
        status_led_show_step(STATUS_LED_STEP_GREEN);
    } else if (g_rssi >= MONITOR_RSSI_FAIR) {
        status_led_show_step(STATUS_LED_STEP_YELLOW);
    } else {
        status_led_show_step(STATUS_LED_STEP_RED);
    }
}

/**
 * @brief Format the temperature and humidity LCD line.
 *
 * @param line Pointer to the mutable first-line buffer.
 * @return void
 */
static void monitor_format_temp(char *line) {
    int t = (int)g_reading.temperature_tenths;
    int dec = t % 10;
    if (dec < 0) {
        dec = -dec;
    }
    snprintf(line, DISPLAY_LINE_LEN, "T:%d.%dC H:%u.%u%%", t / 10, dec,
             (unsigned)g_reading.humidity_tenths / 10u,
             (unsigned)g_reading.humidity_tenths % 10u);
}

/**
 * @brief Format the RSSI and SNR LCD line.
 *
 * @param line Pointer to the mutable second-line buffer.
 * @return void
 */
static void monitor_format_signal(char *line) {
    snprintf(line, DISPLAY_LINE_LEN, "Q:%d S:%d", (int)g_rssi, (int)g_snr);
}

/**
 * @brief Render the reading and signal lines onto the 1602 LCD.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_render(void) {
    monitor_format_temp(g_line1);
    monitor_format_signal(g_line2);
    display_render_lines(g_i2c, g_i2c_addr, g_line1, g_line2);
}

/**
 * @brief Sample the DHT11, render the LCD, and schedule the next read.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_read_tick(uint64_t now_us) {
    sensor_result_t rc = sensor_read(&g_reading);
    if (rc == SENSOR_RESULT_OK) {
        printf("DHT t=%d h=%u\n", (int)g_reading.temperature_tenths, (unsigned)g_reading.humidity_tenths);
    } else {
        printf("READ ERR %d\n", (int)rc);
    }
    monitor_apply_leds();
    monitor_render();
    g_next_read_us = now_us + (uint64_t)MONITOR_READ_INTERVAL_MS * 1000u;
}

/**
 * @brief Format the heartbeat JSON body for the latest RSSI.
 *
 * @param frame Pointer to the mutable frame output buffer.
 * @param frame_len Capacity of the frame output buffer in bytes.
 * @return size_t Number of JSON bytes written, or zero on overflow.
 */
static size_t monitor_build_frame(char *frame, size_t frame_len) {
    int written = snprintf(frame, frame_len, "{\"n\":%u,\"s\":%u,\"q\":%d}", (unsigned)PACKET_NODE_ID, (unsigned)g_seq, (int)g_rssi);
    return (written > 0 && (size_t)written < frame_len) ? (size_t)written : 0u;
}

/**
 * @brief Seal the current heartbeat body into a hex envelope.
 *
 * @param hex Pointer to the NUL-terminated hex output buffer.
 * @param hex_len Capacity of the hex output buffer in bytes.
 * @return bool true when the heartbeat was sealed and encoded.
 */
static bool monitor_seal_frame(char *hex, size_t hex_len) {
    char frame[PICOKIT_26_LORA_RSSI_FRAME_SIZE];
    uint8_t nonce[ENVELOPE_NONCE_LEN];
    uint8_t ad = (uint8_t)PACKET_NODE_ID;
    size_t frame_len = monitor_build_frame(frame, sizeof(frame));
    envelope_fill_nonce(nonce);
    return envelope_seal_hex(g_key, nonce, &ad, 1u, (const uint8_t *)frame, frame_len, hex, hex_len);
}

/**
 * @brief Build and transmit the authenticated heartbeat frame.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_transmit(void) {
    char hex[ENVELOPE_MAX_HEX_LEN];
    if (!g_key_ready) {
        return;
    }
    if (monitor_seal_frame(hex, sizeof(hex))) {
        radio_send_frame(PICOKIT_26_LORA_RSSI_UART, (const uint8_t *)hex, strlen(hex));
        g_seq += 1u;
    }
}

/**
 * @brief Transmit one heartbeat and schedule the next transmit.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_tx_tick(uint64_t now_us) {
    monitor_heartbeat();
    monitor_transmit();
    g_next_tx_us = now_us + (uint64_t)PICOKIT_26_LORA_RSSI_TX_INTERVAL_MS * 1000u;
}

/**
 * @brief Drain inbound radio lines and store the latest signal quality.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_rx_tick(void) {
    radio_rcv_t rcv;
    while (radio_line_pump(PICOKIT_26_LORA_RSSI_UART, g_rx_line, &g_rx_len)) {
        if (radio_parse_rcv(g_rx_line, &rcv) == RADIO_RESULT_OK) {
            g_rssi = rcv.rssi;
            g_snr = rcv.snr;
            printf("RX from 0x%04X q=%d s=%d\n", (unsigned)rcv.sender, (int)g_rssi, (int)g_snr);
        }
    }
}

/**
 * @brief Service the DHT11 sample and heartbeat transmit timers.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_service_timers(uint64_t now_us) {
    if (now_us >= g_next_read_us) {
        monitor_read_tick(now_us);
    }
    if (now_us >= g_next_tx_us) {
        monitor_tx_tick(now_us);
    }
}

bool monitor_init(void) {
    bool ok;
    monitor_bus_init();
    ok = status_led_init() && radio_init(PICOKIT_26_LORA_RSSI_UART) && sensor_init();
    monitor_state_init_io();
    monitor_state_init();
    return ok && display_init(PICOKIT_26_LORA_RSSI_I2C, PICOKIT_26_LORA_RSSI_LCD_ADDR) && monitor_finish();
}

void monitor_deinit(void) {
    g_ready = false;
}

bool monitor_step(void) {
    uint64_t now_us;
    if (!g_ready) {
        return false;
    }
    now_us = time_us_64();
    monitor_service_timers(now_us);
    monitor_rx_tick();
    return true;
}
