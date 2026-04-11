#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

#include "firmware_state.h"

void clearChecksumInEEPROM();
void saveChecksumToEEPROM(const String& checksum, bool commit = true);
void saveWakeCounterToEEPROM(uint32_t counter, bool commit = true);
uint32_t loadWakeCounterFromEEPROM();
String loadChecksumFromEEPROM();
void loadPersistedState(FirmwareState& state);
void savePersistedState(const FirmwareState& state);
void clearPersistedImageState(FirmwareState& state);
void saveRefreshIntervalToEEPROM(uint32_t intervalSeconds, bool commit = true);
uint32_t loadRefreshIntervalFromEEPROM();
void saveElapsedFullFetchSecondsToEEPROM(uint32_t elapsedSeconds, bool commit = true);
uint32_t loadElapsedFullFetchSecondsFromEEPROM();

#endif
