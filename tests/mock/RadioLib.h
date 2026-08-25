/*
 * MOCK. Not RadioLib.
 *
 * This is the exact API surface port_radiolib.cpp depends on, and
 * nothing else. Two purposes:
 *
 *  1. The adapter can be compiled and warning-checked on a host with no
 *     RadioLib installed, which is where CI runs.
 *  2. It documents, in one place, what a RadioLib version bump has to
 *     keep providing. If the real library changes a signature, this file
 *     is the checklist.
 *
 * What it does NOT do is prove the adapter matches the real RadioLib. I
 * wrote both sides from the same understanding, so they agree with each
 * other by construction. The first build against the genuine library is
 * still the real test.
 */
#ifndef LORAITP_MOCK_RADIOLIB_H
#define LORAITP_MOCK_RADIOLIB_H

#include <stddef.h>
#include <stdint.h>

#define RADIOLIB_ERR_NONE           0
#define RADIOLIB_ERR_RX_TIMEOUT   (-6)
#define RADIOLIB_ERR_CRC_MISMATCH (-7)
#define RADIOLIB_ERR_TX_TIMEOUT   (-5)

/*
 * Test hooks. The real library has nothing like these; they exist so a
 * host test can drive the adapter's DIO1 paths deterministically.
 */
void mock_set_rx_frame(const uint8_t *data, size_t len);
void mock_reset();
unsigned mock_tx_count();

class Module {
public:
    Module(uint32_t cs, uint32_t irq, uint32_t rst, uint32_t gpio);
};

class SX1262 {
public:
    explicit SX1262(Module *mod);

    int16_t begin(float freq, float bw, uint8_t sf, uint8_t cr,
                  uint8_t syncWord, int8_t power, uint16_t preambleLength,
                  float tcxoVoltage, bool useRegulatorLDO);

    int16_t setFrequency(float freq);
    int16_t setBandwidth(float bw);
    int16_t setSpreadingFactor(uint8_t sf);
    int16_t setCodingRate(uint8_t cr);
    int16_t setSyncWord(uint8_t syncWord);
    int16_t setPreambleLength(size_t len);
    int16_t setOutputPower(int8_t power);
    int16_t setCurrentLimit(float limit);
    int16_t explicitHeader();
    int16_t setCRC(uint8_t len);

    int16_t setDio2AsRfSwitch(bool enable);
    void    setRfSwitchPins(uint32_t rxEn, uint32_t txEn);
    void    setDio1Action(void (*func)(void));

    int16_t startTransmit(uint8_t *data, size_t len);
    int16_t finishTransmit();
    int16_t startReceive();
    size_t  getPacketLength(bool update = true);
    int16_t readData(uint8_t *data, size_t len);

    float   getRSSI();
    float   getSNR();
    uint8_t randomByte();

    int16_t standby();
    int16_t sleep(bool retainConfig = true);
};

#endif
