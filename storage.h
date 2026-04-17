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
void saveElapsedFullFetchSecondsToEEPROM(uint32_t elapsedSeconds);
uint32_t loadElapsedFullFetchSecondsFromEEPROM();
void saveErrorToEEPROM(ErrorCode errorCode, uint32_t timestamp);
void loadErrorFromEEPROM(ErrorCode& errorCode, uint32_t& timestamp);
void incrementErrorCountInEEPROM();
uint16_t loadErrorCountFromEEPROM();
bool isChecksumValid(const String& checksum);

#endif
