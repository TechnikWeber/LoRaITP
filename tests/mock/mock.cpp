/* Definitions for the mock declarations, so the adapter links. */
#include "Arduino.h"
#include "RadioLib.h"

SPIClass SPI;
void SPIClass::begin(int8_t, int8_t, int8_t, int8_t) {}

extern "C" {
static uint32_t g_ms = 0;
uint32_t millis(void) { return ++g_ms; }
uint32_t micros(void) { return g_ms * 1000u; }
void delay(uint32_t ms) { g_ms += ms; }
void yield(void) { ++g_ms; }
}

static void (*g_dio1_cb)(void) = nullptr;
static uint32_t g_rfsw_rx = 0, g_rfsw_tx = 0;
static bool g_dio2_rfsw = false;
static uint8_t g_rx[256];
static size_t g_rx_len = 0;
static unsigned g_tx_count = 0;

void mock_set_rx_frame(const uint8_t *data, size_t len)
{
    g_rx_len = len > sizeof(g_rx) ? sizeof(g_rx) : len;
    for (size_t i = 0; i < g_rx_len; i++) g_rx[i] = data[i];
}

void mock_reset() { g_rx_len = 0; g_tx_count = 0; }
unsigned mock_tx_count() { return g_tx_count; }

Module::Module(uint32_t, uint32_t, uint32_t, uint32_t) {}
SX1262::SX1262(Module *) {}
int16_t SX1262::begin(float, float, uint8_t, uint8_t, uint8_t, int8_t,
                      uint16_t, float, bool) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setFrequency(float) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setBandwidth(float) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setSpreadingFactor(uint8_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setCodingRate(uint8_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setSyncWord(uint8_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setPreambleLength(size_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setOutputPower(int8_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setCurrentLimit(float) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::explicitHeader() { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setCRC(uint8_t) { return RADIOLIB_ERR_NONE; }
int16_t SX1262::setDio2AsRfSwitch(bool en) { g_dio2_rfsw = en; return RADIOLIB_ERR_NONE; }
void SX1262::setRfSwitchPins(uint32_t rxEn, uint32_t txEn)
{ g_rfsw_rx = rxEn; g_rfsw_tx = txEn; }
uint32_t mock_rfsw_tx() { return g_rfsw_tx; }
uint32_t mock_rfsw_rx() { return g_rfsw_rx; }
bool mock_dio2_rfsw() { return g_dio2_rfsw; }
void SX1262::setDio1Action(void (*cb)(void)) { g_dio1_cb = cb; }
int16_t SX1262::startTransmit(uint8_t *, size_t len)
{
    /* Model the air time so the adapter's measurement has something to
     * measure, then raise TxDone the way the real DIO1 line would. */
    ++g_tx_count;
    delay(10 + (uint32_t)len / 8u);
    if (g_dio1_cb) g_dio1_cb();
    return RADIOLIB_ERR_NONE;
}
int16_t SX1262::finishTransmit() { return RADIOLIB_ERR_NONE; }
int16_t SX1262::startReceive()
{
    if (g_rx_len && g_dio1_cb) g_dio1_cb();   /* RxDone */
    return RADIOLIB_ERR_NONE;
}
size_t SX1262::getPacketLength(bool) { return g_rx_len; }
int16_t SX1262::readData(uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && i < g_rx_len; i++) data[i] = g_rx[i];
    g_rx_len = 0;
    return RADIOLIB_ERR_NONE;
}
float SX1262::getRSSI() { return -98.0f; }
float SX1262::getSNR() { return -5.0f; }
uint8_t SX1262::randomByte() { return 0x5A; }
int16_t SX1262::standby() { return RADIOLIB_ERR_NONE; }
int16_t SX1262::sleep(bool) { return RADIOLIB_ERR_NONE; }
