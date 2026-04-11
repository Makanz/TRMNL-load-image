#include "storage.h"

#include <EEPROM.h>

void clearChecksumInEEPROM() {
  EEPROM.write(0, '\0');
  EEPROM.commit();
}

void saveChecksumToEEPROM(const String& checksum) {
  for (int i = 0; i < (int)EEPROM_CHECKSUM_MAX_LEN && i < (int)checksum.length(); i++) {
    EEPROM.write(i, checksum[i]);
  }
  EEPROM.write(EEPROM_CHECKSUM_MAX_LEN, '\0');
  EEPROM.commit();
}

void saveWakeCounterToEEPROM(uint32_t counter) {
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET, (counter >> 24) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 1, (counter >> 16) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 2, (counter >> 8) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 3, counter & 0xFF);
  EEPROM.commit();
}

uint32_t loadWakeCounterFromEEPROM() {
  uint32_t counter = 0;
  counter = ((uint32_t)EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET) << 24) |
            ((uint32_t)EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 1) << 16) |
            ((uint32_t)EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 2) << 8) |
            (uint32_t)EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 3);
  return counter;
}

String loadChecksumFromEEPROM() {
  String checksum = "";

  for (int i = 0; i < (int)EEPROM_CHECKSUM_MAX_LEN; i++) {
    char value = EEPROM.read(i);
    if (value == '\0') {
      break;
    }
    checksum += value;
  }

  return checksum;
}

void loadPersistedState(FirmwareState& state) {
  state.storedChecksum = loadChecksumFromEEPROM();
  state.wakeCounter = loadWakeCounterFromEEPROM();
  state.refreshIntervalSeconds = loadRefreshIntervalFromEEPROM();
  state.elapsedFullFetchSeconds = loadElapsedFullFetchSecondsFromEEPROM();

  if (!isRefreshIntervalValid(state.refreshIntervalSeconds)) {
    state.refreshIntervalSeconds = 0;
    saveRefreshIntervalToEEPROM(0);
  }

  if (state.elapsedFullFetchSeconds > getFullFetchIntervalSeconds()) {
    state.elapsedFullFetchSeconds = 0;
    saveElapsedFullFetchSecondsToEEPROM(0);
  }
}

void clearPersistedImageState(FirmwareState& state) {
  state.storedChecksum = "";
  state.previousChecksum = "";
  state.wakeCounter = 0;
  state.refreshIntervalSeconds = 0;
  state.elapsedFullFetchSeconds = 0;

  clearChecksumInEEPROM();
  saveWakeCounterToEEPROM(0);
  saveRefreshIntervalToEEPROM(0);
  saveElapsedFullFetchSecondsToEEPROM(0);
}

void saveRefreshIntervalToEEPROM(uint32_t intervalSeconds) {
  EEPROM.write(EEPROM_REFRESH_INTERVAL_OFFSET, (intervalSeconds >> 24) & 0xFF);
  EEPROM.write(EEPROM_REFRESH_INTERVAL_OFFSET + 1, (intervalSeconds >> 16) & 0xFF);
  EEPROM.write(EEPROM_REFRESH_INTERVAL_OFFSET + 2, (intervalSeconds >> 8) & 0xFF);
  EEPROM.write(EEPROM_REFRESH_INTERVAL_OFFSET + 3, intervalSeconds & 0xFF);
  EEPROM.commit();
}

uint32_t loadRefreshIntervalFromEEPROM() {
  uint32_t interval = 0;
  interval = ((uint32_t)EEPROM.read(EEPROM_REFRESH_INTERVAL_OFFSET) << 24) |
             ((uint32_t)EEPROM.read(EEPROM_REFRESH_INTERVAL_OFFSET + 1) << 16) |
             ((uint32_t)EEPROM.read(EEPROM_REFRESH_INTERVAL_OFFSET + 2) << 8) |
             (uint32_t)EEPROM.read(EEPROM_REFRESH_INTERVAL_OFFSET + 3);
  return interval;
}

void saveElapsedFullFetchSecondsToEEPROM(uint32_t elapsedSeconds) {
  EEPROM.write(EEPROM_FULL_FETCH_ELAPSED_OFFSET, (elapsedSeconds >> 24) & 0xFF);
  EEPROM.write(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 1, (elapsedSeconds >> 16) & 0xFF);
  EEPROM.write(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 2, (elapsedSeconds >> 8) & 0xFF);
  EEPROM.write(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 3, elapsedSeconds & 0xFF);
  EEPROM.commit();
}

uint32_t loadElapsedFullFetchSecondsFromEEPROM() {
  uint32_t elapsedSeconds = 0;
  elapsedSeconds = ((uint32_t)EEPROM.read(EEPROM_FULL_FETCH_ELAPSED_OFFSET) << 24) |
                   ((uint32_t)EEPROM.read(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 1) << 16) |
                   ((uint32_t)EEPROM.read(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 2) << 8) |
                   (uint32_t)EEPROM.read(EEPROM_FULL_FETCH_ELAPSED_OFFSET + 3);
  return elapsedSeconds;
}
