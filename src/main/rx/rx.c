/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <string.h>

#include "platform.h"

#include "common/maths.h"

#include "config/config.h"

#include "drivers/serial.h"
#include "drivers/adc.h"
#include "io/serial.h"

#include "flight/failsafe.h"

#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"
#include "rx/pwm.h"
#include "rx/sbus.h"
#include "rx/spektrum.h"
#include "rx/sumd.h"
#include "rx/msp.h"

#include "rx/rx.h"

extern int16_t debug[4];

void rxPwmInit(rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback);

bool sbusInit(rxConfig_t *initialRxConfig, rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback);
bool spektrumInit(rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback);
bool sumdInit(rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback);

bool rxMspInit(rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback);

const char rcChannelLetters[] = "AERT1234";

uint16_t rssi;                  // range: [0;1023]

int16_t rcData[MAX_SUPPORTED_RC_CHANNEL_COUNT];     // interval [1000;2000]

#define PPM_AND_PWM_SAMPLE_COUNT 4

#define PULSE_MIN   750       // minimum PWM pulse width which is considered valid
#define PULSE_MAX   2250      // maximum PWM pulse width which is considered valid

#define DELAY_50_HZ (1000000 / 50)

static rcReadRawDataPtr rcReadRawFunc = NULL;  // receive data from default (pwm/ppm) or additional (spek/sbus/?? receiver drivers)

rxRuntimeConfig_t rxRuntimeConfig;
static rxConfig_t *rxConfig;

void serialRxInit(rxConfig_t *rxConfig);

static failsafe_t *failsafe;

void useRxConfig(rxConfig_t *rxConfigToUse)
{
    rxConfig = rxConfigToUse;
}

#ifdef SERIAL_RX
void updateSerialRxFunctionConstraint(functionConstraint_t *functionConstraintToUpdate)
{
    switch (rxConfig->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
        case SERIALRX_SPEKTRUM2048:
            spektrumUpdateSerialRxFunctionConstraint(functionConstraintToUpdate);
            break;
        case SERIALRX_SBUS:
            sbusUpdateSerialRxFunctionConstraint(functionConstraintToUpdate);
            break;
        case SERIALRX_SUMD:
            sumdUpdateSerialRxFunctionConstraint(functionConstraintToUpdate);
            break;
    }
}
#endif

void rxInit(rxConfig_t *rxConfig, failsafe_t *initialFailsafe)
{
    uint8_t i;
    useRxConfig(rxConfig);

    for (i = 0; i < MAX_SUPPORTED_RC_CHANNEL_COUNT; i++) {
        rcData[i] = rxConfig->midrc;
    }

    failsafe = initialFailsafe;

#ifdef SERIAL_RX
    if (feature(FEATURE_RX_SERIAL)) {
        serialRxInit(rxConfig);
    }
#endif

    if (feature(FEATURE_RX_MSP)) {
        rxMspInit(rxConfig, &rxRuntimeConfig, &rcReadRawFunc);
    }

    if (feature(FEATURE_RX_PPM) || feature(FEATURE_RX_PARALLEL_PWM)) {
        rxPwmInit(&rxRuntimeConfig, &rcReadRawFunc);
    }
}

#ifdef SERIAL_RX
void serialRxInit(rxConfig_t *rxConfig)
{
    bool enabled = false;
    switch (rxConfig->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
        case SERIALRX_SPEKTRUM2048:
            enabled = spektrumInit(rxConfig, &rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SBUS:
            enabled = sbusInit(rxConfig, &rxRuntimeConfig, &rcReadRawFunc);
            break;
        case SERIALRX_SUMD:
            enabled = sumdInit(rxConfig, &rxRuntimeConfig, &rcReadRawFunc);
            break;
    }

    if (!enabled) {
        featureClear(FEATURE_RX_SERIAL);
        rcReadRawFunc = NULL;
    }
}

bool isSerialRxFrameComplete(rxConfig_t *rxConfig)
{
    switch (rxConfig->serialrx_provider) {
        case SERIALRX_SPEKTRUM1024:
        case SERIALRX_SPEKTRUM2048:
            return spektrumFrameComplete();
        case SERIALRX_SBUS:
            return sbusFrameComplete();
        case SERIALRX_SUMD:
            return sumdFrameComplete();
    }
    return false;
}
#endif

uint8_t calculateChannelRemapping(uint8_t *channelMap, uint8_t channelMapEntryCount, uint8_t channelToRemap)
{
    if (channelToRemap < channelMapEntryCount) {
        return channelMap[channelToRemap];
    }
    return channelToRemap;
}

static bool rcDataReceived = false;
static uint32_t rxUpdateAt = 0;


void updateRx(void)
{
    rcDataReceived = false;

#ifdef SERIAL_RX
    // calculate rc stuff from serial-based receivers (spek/sbus)
    if (feature(FEATURE_RX_SERIAL)) {
        rcDataReceived = isSerialRxFrameComplete(rxConfig);
    }
#endif

    if (feature(FEATURE_RX_MSP)) {
        rcDataReceived = rxMspFrameComplete();
    }

    if (rcDataReceived) {
        if (feature(FEATURE_FAILSAFE)) {
            failsafe->vTable->reset();
        }
    }
}

bool shouldProcessRx(uint32_t currentTime)
{
    return rcDataReceived || ((int32_t)(currentTime - rxUpdateAt) >= 0); // data driven or 50Hz
}

static bool isRxDataDriven(void) {
    return !(feature(FEATURE_RX_PARALLEL_PWM | FEATURE_RX_PPM));
}

static uint8_t rcSampleIndex = 0;

uint16_t calculateNonDataDrivenChannel(uint8_t chan, uint16_t sample)
{
    static int16_t rcSamples[MAX_SUPPORTED_RX_PARALLEL_PWM_OR_PPM_CHANNEL_COUNT][PPM_AND_PWM_SAMPLE_COUNT];
    static int16_t rcDataMean[MAX_SUPPORTED_RX_PARALLEL_PWM_OR_PPM_CHANNEL_COUNT];
    static bool rxSamplesCollected = false;

    uint8_t currentSampleIndex = rcSampleIndex % PPM_AND_PWM_SAMPLE_COUNT;

    // update the recent samples and compute the average of them
    rcSamples[chan][currentSampleIndex] = sample;

    // avoid returning an incorrect average which would otherwise occur before enough samples
    if (!rxSamplesCollected) {
        if (rcSampleIndex < PPM_AND_PWM_SAMPLE_COUNT) {
            return sample;
        }
        rxSamplesCollected = true;
    }

    rcDataMean[chan] = 0;

    uint8_t sampleIndex;
    for (sampleIndex = 0; sampleIndex < PPM_AND_PWM_SAMPLE_COUNT; sampleIndex++)
        rcDataMean[chan] += rcSamples[chan][sampleIndex];

    return rcDataMean[chan] / PPM_AND_PWM_SAMPLE_COUNT;
}

void processRxChannels(void)
{
    uint8_t chan;

    bool shouldCheckPulse = true;

    if (feature(FEATURE_FAILSAFE | FEATURE_RX_PPM)) {
        shouldCheckPulse = isPPMDataBeingReceived();
        resetPPMDataReceivedState();
    }

    for (chan = 0; chan < rxRuntimeConfig.channelCount; chan++) {

        if (!rcReadRawFunc) {
            rcData[chan] = rxConfig->midrc;
            continue;
        }

        uint8_t rawChannel = calculateChannelRemapping(rxConfig->rcmap, REMAPPABLE_CHANNEL_COUNT, chan);

        // sample the channel
        uint16_t sample = rcReadRawFunc(&rxRuntimeConfig, rawChannel);

        if (feature(FEATURE_FAILSAFE) && shouldCheckPulse) {
            failsafe->vTable->checkPulse(rawChannel, sample);
        }

        // validate the range
        if (sample < PULSE_MIN || sample > PULSE_MAX)
            sample = rxConfig->midrc;

        if (isRxDataDriven()) {
            rcData[chan] = sample;
        } else {
            rcData[chan] = calculateNonDataDrivenChannel(chan, sample);
        }
    }
}

void processDataDrivenRx(void)
{
    if (!rcDataReceived) {
        return;
    }

    failsafe->vTable->reset();

    processRxChannels();

    rcDataReceived = false;
}

void processNonDataDrivenRx(void)
{
    rcSampleIndex++;

    processRxChannels();
}

void calculateRxChannelsAndUpdateFailsafe(uint32_t currentTime)
{
    rxUpdateAt = currentTime + DELAY_50_HZ;

    if (feature(FEATURE_FAILSAFE)) {
        failsafe->vTable->incrementCounter();
    }

    if (isRxDataDriven()) {
        processDataDrivenRx();
    } else {
        processNonDataDrivenRx();
    }
}

void parseRcChannels(const char *input, rxConfig_t *rxConfig)
{
    const char *c, *s;

    for (c = input; *c; c++) {
        s = strchr(rcChannelLetters, *c);
        if (s)
            rxConfig->rcmap[s - rcChannelLetters] = c - input;
    }
}

void updateRSSIPWM(void)
{
    int16_t pwmRssi = 0;
    // Read value of AUX channel as rssi
    pwmRssi = rcData[rxConfig->rssi_channel - 1];


    // Range of rawPwmRssi is [1000;2000]. rssi should be in [0;1023];
    rssi = (uint16_t)((constrain(pwmRssi - 1000, 0, 1000) / 1000.0f) * 1023.0f);
}

#define RSSI_ADC_SAMPLE_COUNT 16
#define RSSI_SCALE (0xFFF / 100.0f)

void updateRSSIADC(uint32_t currentTime)
{
    static uint8_t adcRssiSamples[RSSI_ADC_SAMPLE_COUNT];
    static uint8_t adcRssiSampleIndex = 0;
    static uint32_t rssiUpdateAt = 0;

    if ((int32_t)(currentTime - rssiUpdateAt) < 0) {
        return;
    }
    rssiUpdateAt = currentTime + DELAY_50_HZ;

    int16_t adcRssiMean = 0;
    uint16_t adcRssiSample = adcGetChannel(ADC_RSSI);
    uint8_t rssiPercentage = adcRssiSample / RSSI_SCALE;

    adcRssiSampleIndex = (adcRssiSampleIndex + 1) % RSSI_ADC_SAMPLE_COUNT;

    adcRssiSamples[adcRssiSampleIndex] = rssiPercentage;

    uint8_t sampleIndex;

    for (sampleIndex = 0; sampleIndex < RSSI_ADC_SAMPLE_COUNT; sampleIndex++) {
        adcRssiMean += adcRssiSamples[sampleIndex];
    }

    adcRssiMean = adcRssiMean / RSSI_ADC_SAMPLE_COUNT;

    rssi = (uint16_t)((constrain(adcRssiMean, 0, 100) / 100.0f) * 1023.0f);
}

void updateRSSI(uint32_t currentTime)
{

    if (rxConfig->rssi_channel > 0) {
        updateRSSIPWM();
    } else if (feature(FEATURE_RSSI_ADC)) {
        updateRSSIADC(currentTime);
    }
}


