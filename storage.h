#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

#include "firmware_state.h"

void clearChecksumInEEPROM();
void saveChecksumToEEPROM(const String& checksum);
void saveWakeCounterToEEPROM(uint32_t counter);
uint32_t loadWakeCounterFromEEPROM();
String loadChecksumFromEEPROM();
void loadPersistedState(FirmwareState& state);
void clearPersistedImageState(FirmwareState& state);
void saveRefreshIntervalToEEPROM(uint32_t intervalSeconds);
uint32_t loadRefreshIntervalFromEEPROM();

#endif
