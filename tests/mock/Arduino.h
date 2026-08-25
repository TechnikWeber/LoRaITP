/*
 * MOCK. Not Arduino.
 *
 * The minimum surface port_radiolib.cpp uses, so the adapter can be
 * compiled on a host that has no Arduino core. Compiling against this
 * proves the adapter is internally consistent and free of syntax errors.
 * It does NOT prove the real Arduino API matches - it cannot, because I
 * wrote both sides. Treat a green build here as "worth trying on
 * hardware", not as "verified".
 */
#ifndef LORAITP_MOCK_ARDUINO_H
#define LORAITP_MOCK_ARDUINO_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);
void     yield(void);
}

#define IRAM_ATTR

class SPIClass {
public:
    void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1,
               int8_t ss = -1);
};
extern SPIClass SPI;

#endif
