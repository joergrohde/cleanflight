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
#include <string.h>

#include "platform.h"

#include "build_config.h"

#include "common/axis.h"
#include "flight/flight.h"

#include "drivers/accgyro.h"
#include "drivers/system.h"
#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"

#include "sensors/sensors.h"
#include "sensors/gyro.h"

#include "io/statusindicator.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "drivers/serial.h"
#include "io/serial.h"
#include "telemetry/telemetry.h"

#include "flight/mixer.h"
#include "sensors/boardalignment.h"
#include "sensors/battery.h"
#include "io/gimbal.h"
#include "io/escservo.h"
#include "rx/rx.h"
#include "io/rc_controls.h"
#include "io/rc_curves.h"
#include "io/gps.h"
#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/navigation.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_profile.h"
#include "config/config_master.h"

#define BRUSHED_MOTORS_PWM_RATE 16000
#define BRUSHLESS_MOTORS_PWM_RATE 400

void setPIDController(int type); // FIXME PID code needs to be in flight_pid.c/h
void mixerUseConfigs(servoParam_t *servoConfToUse, flight3DConfig_t *flight3DConfigToUse,
        escAndServoConfig_t *escAndServoConfigToUse, mixerConfig_t *mixerConfigToUse,
        airplaneConfig_t *airplaneConfigToUse, rxConfig_t *rxConfig, gimbalConfig_t *gimbalConfigToUse);

#define FLASH_TO_RESERVE_FOR_CONFIG 0x800

#ifdef STM32F303xC
#define FLASH_PAGE_COUNT 128
#define FLASH_PAGE_SIZE                 ((uint16_t)0x800)
#endif

#ifndef FLASH_PAGE_COUNT
#define FLASH_PAGE_COUNT 128
#define FLASH_PAGE_SIZE                 ((uint16_t)0x400)
#endif

// use the last flash pages for storage
static uint32_t flashWriteAddress = (0x08000000 + (uint32_t)((FLASH_PAGE_SIZE * FLASH_PAGE_COUNT) - FLASH_TO_RESERVE_FOR_CONFIG));

master_t masterConfig;      // master config struct with data independent from profiles
profile_t currentProfile;   // profile config struct

static const uint8_t EEPROM_CONF_VERSION = 75;

static void resetAccelerometerTrims(flightDynamicsTrims_t *accelerometerTrims)
{
    accelerometerTrims->values.pitch = 0;
    accelerometerTrims->values.roll = 0;
    accelerometerTrims->values.yaw = 0;
}

static void resetPidProfile(pidProfile_t *pidProfile)
{
    pidProfile->P8[ROLL] = 40;
    pidProfile->I8[ROLL] = 30;
    pidProfile->D8[ROLL] = 23;
    pidProfile->P8[PITCH] = 40;
    pidProfile->I8[PITCH] = 30;
    pidProfile->D8[PITCH] = 23;
    pidProfile->P8[YAW] = 85;
    pidProfile->I8[YAW] = 45;
    pidProfile->D8[YAW] = 0;
    pidProfile->P8[PIDALT] = 50;
    pidProfile->I8[PIDALT] = 0;
    pidProfile->D8[PIDALT] = 0;
    pidProfile->P8[PIDPOS] = 11; // POSHOLD_P * 100;
    pidProfile->I8[PIDPOS] = 0; // POSHOLD_I * 100;
    pidProfile->D8[PIDPOS] = 0;
    pidProfile->P8[PIDPOSR] = 20; // POSHOLD_RATE_P * 10;
    pidProfile->I8[PIDPOSR] = 8; // POSHOLD_RATE_I * 100;
    pidProfile->D8[PIDPOSR] = 45; // POSHOLD_RATE_D * 1000;
    pidProfile->P8[PIDNAVR] = 14; // NAV_P * 10;
    pidProfile->I8[PIDNAVR] = 20; // NAV_I * 100;
    pidProfile->D8[PIDNAVR] = 80; // NAV_D * 1000;
    pidProfile->P8[PIDLEVEL] = 90;
    pidProfile->I8[PIDLEVEL] = 10;
    pidProfile->D8[PIDLEVEL] = 100;
    pidProfile->P8[PIDMAG] = 40;
    pidProfile->P8[PIDVEL] = 120;
    pidProfile->I8[PIDVEL] = 45;
    pidProfile->D8[PIDVEL] = 1;

    pidProfile->P_f[ROLL] = 2.5f;     // new PID with preliminary defaults test carefully
    pidProfile->I_f[ROLL] = 0.6f;
    pidProfile->D_f[ROLL] = 0.06f;
    pidProfile->P_f[PITCH] = 2.5f;
    pidProfile->I_f[PITCH] = 0.6f;
    pidProfile->D_f[PITCH] = 0.06f;
    pidProfile->P_f[YAW] = 8.0f;
    pidProfile->I_f[YAW] = 0.5f;
    pidProfile->D_f[YAW] = 0.05f;
    pidProfile->A_level = 5.0f;
    pidProfile->H_level = 3.0f;
}

#ifdef GPS
void resetGpsProfile(gpsProfile_t *gpsProfile)
{
    gpsProfile->gps_wp_radius = 200;
    gpsProfile->gps_lpf = 20;
    gpsProfile->nav_slew_rate = 30;
    gpsProfile->nav_controls_heading = 1;
    gpsProfile->nav_speed_min = 100;
    gpsProfile->nav_speed_max = 300;
    gpsProfile->ap_mode = 40;
}
#endif

void resetBarometerConfig(barometerConfig_t *barometerConfig)
{
    barometerConfig->baro_sample_count = 21;
    barometerConfig->baro_noise_lpf = 0.6f;
    barometerConfig->baro_cf_vel = 0.985f;
    barometerConfig->baro_cf_alt = 0.965f;
}

void resetSensorAlignment(sensorAlignmentConfig_t *sensorAlignmentConfig)
{
    sensorAlignmentConfig->gyro_align = ALIGN_DEFAULT;
    sensorAlignmentConfig->acc_align = ALIGN_DEFAULT;
    sensorAlignmentConfig->mag_align = ALIGN_DEFAULT;
}

void resetEscAndServoConfig(escAndServoConfig_t *escAndServoConfig)
{
    escAndServoConfig->minthrottle = 1150;
    escAndServoConfig->maxthrottle = 1850;
    escAndServoConfig->mincommand = 1000;
}

void resetFlight3DConfig(flight3DConfig_t *flight3DConfig)
{
    flight3DConfig->deadband3d_low = 1406;
    flight3DConfig->deadband3d_high = 1514;
    flight3DConfig->neutral3d = 1460;
    flight3DConfig->deadband3d_throttle = 50;
}

void resetTelemetryConfig(telemetryConfig_t *telemetryConfig)
{
    telemetryConfig->telemetry_provider = TELEMETRY_PROVIDER_FRSKY;
    telemetryConfig->frsky_inversion = SERIAL_NOT_INVERTED;
    telemetryConfig->telemetry_switch = 0;
}

void resetSerialConfig(serialConfig_t *serialConfig)
{
    serialConfig->serial_port_scenario[0] = lookupScenarioIndex(SCENARIO_MSP_CLI_TELEMETRY_GPS_PASTHROUGH);
    serialConfig->serial_port_scenario[1] = lookupScenarioIndex(SCENARIO_GPS_ONLY);
#if (SERIAL_PORT_COUNT > 2)
    serialConfig->serial_port_scenario[2] = lookupScenarioIndex(SCENARIO_UNUSED);
    serialConfig->serial_port_scenario[3] = lookupScenarioIndex(SCENARIO_UNUSED);

#if (SERIAL_PORT_COUNT > 4)
    serialConfig->serial_port_scenario[4] = lookupScenarioIndex(SCENARIO_UNUSED);
#endif
#endif

    serialConfig->msp_baudrate = 115200;
    serialConfig->cli_baudrate = 115200;
    serialConfig->gps_baudrate = 115200;
    serialConfig->gps_passthrough_baudrate = 115200;

    serialConfig->reboot_character = 'R';
}

// Default settings
static void resetConf(void)
{
    int i;
    int8_t servoRates[8] = { 30, 30, 100, 100, 100, 100, 100, 100 };

    // Clear all configuration
    memset(&masterConfig, 0, sizeof(master_t));
    memset(&currentProfile, 0, sizeof(profile_t));

    masterConfig.version = EEPROM_CONF_VERSION;
    masterConfig.mixerConfiguration = MULTITYPE_QUADX;
    featureClearAll();
#ifdef CJMCU
    featureSet(FEATURE_RX_PPM);
#endif
    featureSet(FEATURE_VBAT);

    // global settings
    masterConfig.current_profile_index = 0;     // default profile
    masterConfig.gyro_cmpf_factor = 600;        // default MWC
    masterConfig.gyro_cmpfm_factor = 250;       // default MWC
    masterConfig.gyro_lpf = 42;                 // supported by all gyro drivers now. In case of ST gyro, will default to 32Hz instead

    resetAccelerometerTrims(&masterConfig.accZero);

    resetSensorAlignment(&masterConfig.sensorAlignmentConfig);

    masterConfig.boardAlignment.rollDegrees = 0;
    masterConfig.boardAlignment.pitchDegrees = 0;
    masterConfig.boardAlignment.yawDegrees = 0;
    masterConfig.acc_hardware = ACC_DEFAULT;     // default/autodetect
    masterConfig.max_angle_inclination = 500;    // 50 degrees
    masterConfig.yaw_control_direction = 1;
    masterConfig.gyroConfig.gyroMovementCalibrationThreshold = 32;

    masterConfig.batteryConfig.vbatscale = 110;
    masterConfig.batteryConfig.vbatmaxcellvoltage = 43;
    masterConfig.batteryConfig.vbatmincellvoltage = 33;
    masterConfig.batteryConfig.currentMeterOffset = 0;
    masterConfig.batteryConfig.currentMeterScale = 400; // for Allegro ACS758LCB-100U (40mV/A)

    resetTelemetryConfig(&masterConfig.telemetryConfig);

    masterConfig.rxConfig.serialrx_provider = 0;
    masterConfig.rxConfig.midrc = 1500;
    masterConfig.rxConfig.mincheck = 1100;
    masterConfig.rxConfig.maxcheck = 1900;
    masterConfig.rxConfig.rssi_channel = 0;

    masterConfig.inputFilteringMode = INPUT_FILTERING_DISABLED;

    masterConfig.retarded_arm = 0;              // disable arm/disarm on roll left/right
    masterConfig.small_angle = 25;

    masterConfig.airplaneConfig.flaps_speed = 0;
    masterConfig.fixedwing_althold_dir = 1;

    // Motor/ESC/Servo
    resetEscAndServoConfig(&masterConfig.escAndServoConfig);
    resetFlight3DConfig(&masterConfig.flight3DConfig);

#ifdef BRUSHED_MOTORS
    masterConfig.motor_pwm_rate = BRUSHED_MOTORS_PWM_RATE;
#else
    masterConfig.motor_pwm_rate = BRUSHLESS_MOTORS_PWM_RATE;
#endif
    masterConfig.servo_pwm_rate = 50;

#ifdef GPS
    // gps/nav stuff
    masterConfig.gpsConfig.provider = GPS_NMEA;
    masterConfig.gpsConfig.sbasMode = SBAS_AUTO;
#endif

    resetSerialConfig(&masterConfig.serialConfig);

    masterConfig.looptime = 3500;
    masterConfig.emf_avoidance = 0;

    currentProfile.pidController = 0;
    resetPidProfile(&currentProfile.pidProfile);

    currentProfile.controlRateConfig.rcRate8 = 90;
    currentProfile.controlRateConfig.rcExpo8 = 65;
    currentProfile.controlRateConfig.rollPitchRate = 0;
    currentProfile.controlRateConfig.yawRate = 0;
    currentProfile.dynThrPID = 0;
    currentProfile.tpa_breakpoint = 1500;
    currentProfile.controlRateConfig.thrMid8 = 50;
    currentProfile.controlRateConfig.thrExpo8 = 0;

    // for (i = 0; i < CHECKBOXITEMS; i++)
    //     cfg.activate[i] = 0;

    resetRollAndPitchTrims(&currentProfile.accelerometerTrims);

    currentProfile.mag_declination = 0;
    currentProfile.acc_lpf_factor = 4;
    currentProfile.accz_lpf_cutoff = 5.0f;
    currentProfile.accDeadband.xy = 40;
    currentProfile.accDeadband.z = 40;

    resetBarometerConfig(&currentProfile.barometerConfig);

    currentProfile.acc_unarmedcal = 1;

    // Radio
    parseRcChannels("AETR1234", &masterConfig.rxConfig);
    currentProfile.deadband = 0;
    currentProfile.yaw_deadband = 0;
    currentProfile.alt_hold_deadband = 40;
    currentProfile.alt_hold_fast_change = 1;
    currentProfile.throttle_correction_value = 0;      // could 10 with althold or 40 for fpv
    currentProfile.throttle_correction_angle = 800;    // could be 80.0 deg with atlhold or 45.0 for fpv

    // Failsafe Variables
    currentProfile.failsafeConfig.failsafe_delay = 10;              // 1sec
    currentProfile.failsafeConfig.failsafe_off_delay = 200;         // 20sec
    currentProfile.failsafeConfig.failsafe_throttle = 1200;         // decent default which should always be below hover throttle for people.
    currentProfile.failsafeConfig.failsafe_min_usec = 985;          // any of first 4 channels below this value will trigger failsafe
    currentProfile.failsafeConfig.failsafe_max_usec = 2115;         // any of first 4 channels above this value will trigger failsafe

    // servos
    for (i = 0; i < 8; i++) {
        currentProfile.servoConf[i].min = DEFAULT_SERVO_MIN;
        currentProfile.servoConf[i].max = DEFAULT_SERVO_MAX;
        currentProfile.servoConf[i].middle = DEFAULT_SERVO_MIDDLE;
        currentProfile.servoConf[i].rate = servoRates[i];
        currentProfile.servoConf[i].forwardFromChannel = CHANNEL_FORWARDING_DISABLED;
    }

    currentProfile.mixerConfig.yaw_direction = 1;
    currentProfile.mixerConfig.tri_unarmed_servo = 1;

    // gimbal
    currentProfile.gimbalConfig.gimbal_flags = GIMBAL_NORMAL;

#ifdef GPS
    resetGpsProfile(&currentProfile.gpsProfile);
#endif

    // custom mixer. clear by defaults.
    for (i = 0; i < MAX_SUPPORTED_MOTORS; i++)
        masterConfig.customMixer[i].throttle = 0.0f;

    // copy default config into all 3 profiles
    for (i = 0; i < 3; i++)
        memcpy(&masterConfig.profile[i], &currentProfile, sizeof(profile_t));
}

static uint8_t calculateChecksum(const uint8_t *data, uint32_t length)
{
    uint8_t checksum = 0;
    const uint8_t *byteOffset;

    for (byteOffset = data; byteOffset < (data + length); byteOffset++)
        checksum ^= *byteOffset;
    return checksum;
}

static bool isEEPROMContentValid(void)
{
    const master_t *temp = (const master_t *) flashWriteAddress;
    uint8_t checksum = 0;

    // check version number
    if (EEPROM_CONF_VERSION != temp->version)
        return false;

    // check size and magic numbers
    if (temp->size != sizeof(master_t) || temp->magic_be != 0xBE || temp->magic_ef != 0xEF)
        return false;

    // verify integrity of temporary copy
    checksum = calculateChecksum((const uint8_t *) temp, sizeof(master_t));
    if (checksum != 0)
        return false;

    // looks good, let's roll!
    return true;
}

void activateConfig(void)
{
    static imuRuntimeConfig_t imuRuntimeConfig;

    generatePitchCurve(&currentProfile.controlRateConfig);
    generateThrottleCurve(&currentProfile.controlRateConfig, &masterConfig.escAndServoConfig);

    useGyroConfig(&masterConfig.gyroConfig);
#ifdef TELEMETRY
    useTelemetryConfig(&masterConfig.telemetryConfig);
#endif
    setPIDController(currentProfile.pidController);
#ifdef GPS
    gpsUseProfile(&currentProfile.gpsProfile);
    gpsUsePIDs(&currentProfile.pidProfile);
#endif
    useFailsafeConfig(&currentProfile.failsafeConfig);
    setAccelerationTrims(&masterConfig.accZero);
    mixerUseConfigs(
            currentProfile.servoConf,
            &masterConfig.flight3DConfig,
            &masterConfig.escAndServoConfig,
            &currentProfile.mixerConfig,
            &masterConfig.airplaneConfig,
            &masterConfig.rxConfig,
            &currentProfile.gimbalConfig
            );

    imuRuntimeConfig.gyro_cmpf_factor = masterConfig.gyro_cmpf_factor;
    imuRuntimeConfig.gyro_cmpfm_factor = masterConfig.gyro_cmpfm_factor;
    imuRuntimeConfig.acc_lpf_factor = currentProfile.acc_lpf_factor;
    imuRuntimeConfig.acc_unarmedcal = currentProfile.acc_unarmedcal;;
    imuRuntimeConfig.small_angle = masterConfig.small_angle;

    configureImu(&imuRuntimeConfig, &currentProfile.pidProfile, &currentProfile.barometerConfig, &currentProfile.accDeadband);

    calculateThrottleAngleScale(currentProfile.throttle_correction_angle);
    calculateAccZLowPassFilterRCTimeConstant(currentProfile.accz_lpf_cutoff);

#ifdef BARO
    useBarometerConfig(&currentProfile.barometerConfig);
#endif
}

void validateAndFixConfig(void)
{
    if (!(feature(FEATURE_RX_PARALLEL_PWM) || feature(FEATURE_RX_PPM) || feature(FEATURE_RX_SERIAL) || feature(FEATURE_RX_MSP))) {
        featureSet(FEATURE_RX_PARALLEL_PWM); // Consider changing the default to PPM
    }

    if (feature(FEATURE_RX_PPM)) {
        featureClear(FEATURE_RX_PARALLEL_PWM);
    }

    if (feature(FEATURE_RX_MSP)) {
        featureClear(FEATURE_RX_SERIAL);
        featureClear(FEATURE_RX_PARALLEL_PWM);
        featureClear(FEATURE_RX_PPM);
    }

    if (feature(FEATURE_RX_SERIAL)) {
        featureClear(FEATURE_RX_PARALLEL_PWM);
        featureClear(FEATURE_RX_PPM);
    }

    if (feature(FEATURE_RX_PARALLEL_PWM)) {
#if defined(STM32F10X_MD)
        // rssi adc needs the same ports
        featureClear(FEATURE_RSSI_ADC);
        // current meter needs the same ports
        featureClear(FEATURE_CURRENT_METER);
#ifdef SONAR
        // sonar needs a free PWM port
        featureClear(FEATURE_SONAR);
#endif
#endif

#if defined(STM32F10X_MD) || defined(CHEBUZZ) || defined(STM32F3DISCOVERY)
        // led strip needs the same ports
        featureClear(FEATURE_LED_STRIP);
#endif


        // software serial needs free PWM ports
        featureClear(FEATURE_SOFTSERIAL);
    }


#if defined(STM32F10X_MD)
    // led strip needs the same timer as softserial
    if (feature(FEATURE_SOFTSERIAL)) {
        featureClear(FEATURE_LED_STRIP);
    }
#endif


    useRxConfig(&masterConfig.rxConfig);

    serialConfig_t *serialConfig = &masterConfig.serialConfig;
    applySerialConfigToPortFunctions(serialConfig);

    if (!isSerialConfigValid(serialConfig)) {
        resetSerialConfig(serialConfig);
    }
}

void initEEPROM(void)
{
#if defined(STM32F10X_MD)

#define FLASH_SIZE_REGISTER 0x1FFFF7E0

    const uint32_t flashSize = *((uint32_t *)FLASH_SIZE_REGISTER) & 0xFFFF;

    // calculate write address based on contents of Flash size register. Use last 2 kbytes for storage
    flashWriteAddress = 0x08000000 + (FLASH_PAGE_SIZE * (flashSize - 2));
#endif
}

void readEEPROM(void)
{
    // Sanity check
    if (!isEEPROMContentValid())
        failureMode(10);

    // Read flash
    memcpy(&masterConfig, (char *) flashWriteAddress, sizeof(master_t));
    // Copy current profile
    if (masterConfig.current_profile_index > 2) // sanity check
        masterConfig.current_profile_index = 0;
    memcpy(&currentProfile, &masterConfig.profile[masterConfig.current_profile_index], sizeof(profile_t));

    validateAndFixConfig();
    activateConfig();
}

void readEEPROMAndNotify(void)
{
    // re-read written data
    readEEPROM();
    blinkLedAndSoundBeeper(15, 20, 1);
}

void copyCurrentProfileToProfileSlot(uint8_t profileSlotIndex)
{
    // copy current in-memory profile to stored configuration
    memcpy(&masterConfig.profile[profileSlotIndex], &currentProfile, sizeof(profile_t));
}


void writeEEPROM(void)
{
    // Generate compile time error if the config does not fit in the reserved area of flash.
    BUILD_BUG_ON(sizeof(master_t) > FLASH_TO_RESERVE_FOR_CONFIG);

    FLASH_Status status = 0;
    uint32_t wordOffset;
    int8_t attemptsRemaining = 3;

    // prepare checksum/version constants
    masterConfig.version = EEPROM_CONF_VERSION;
    masterConfig.size = sizeof(master_t);
    masterConfig.magic_be = 0xBE;
    masterConfig.magic_ef = 0xEF;
    masterConfig.chk = 0; // erase checksum before recalculating
    masterConfig.chk = calculateChecksum((const uint8_t *) &masterConfig, sizeof(master_t));

    // write it
    FLASH_Unlock();
    while (attemptsRemaining--) {
#ifdef STM32F3DISCOVERY
        FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
#endif
#ifdef STM32F10X_MD
        FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
#endif
        for (wordOffset = 0; wordOffset < sizeof(master_t); wordOffset += 4) {
            if (wordOffset % FLASH_PAGE_SIZE == 0) {
                status = FLASH_ErasePage(flashWriteAddress + wordOffset);
                if (status != FLASH_COMPLETE) {
                    break;
                }
            }

            status = FLASH_ProgramWord(flashWriteAddress + wordOffset,
                    *(uint32_t *) ((char *) &masterConfig + wordOffset));
            if (status != FLASH_COMPLETE) {
                break;
            }
        }
        if (status == FLASH_COMPLETE) {
            break;
        }
    }
    FLASH_Lock();

    // Flash write failed - just die now
    if (status != FLASH_COMPLETE || !isEEPROMContentValid()) {
        failureMode(10);
    }
}

void ensureEEPROMContainsValidData(void)
{
    if (isEEPROMContentValid()) {
        return;
    }

    resetEEPROM();
}
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

void resetEEPROM(void)
{
    resetConf();
    writeEEPROM();
}

void saveAndReloadCurrentProfileToCurrentProfileSlot(void)
{
    copyCurrentProfileToProfileSlot(masterConfig.current_profile_index);
    writeEEPROM();
    readEEPROMAndNotify();
}

void changeProfile(uint8_t profileIndex)
{
    masterConfig.current_profile_index = profileIndex;
    writeEEPROM();
    readEEPROM();
    blinkLedAndSoundBeeper(2, 40, profileIndex + 1);
}

bool feature(uint32_t mask)
{
    return masterConfig.enabledFeatures & mask;
}

void featureSet(uint32_t mask)
{
    masterConfig.enabledFeatures |= mask;
}

void featureClear(uint32_t mask)
{
    masterConfig.enabledFeatures &= ~(mask);
}

void featureClearAll()
{
    masterConfig.enabledFeatures = 0;
}

uint32_t featureMask(void)
{
    return masterConfig.enabledFeatures;
}

