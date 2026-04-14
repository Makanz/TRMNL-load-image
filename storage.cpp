#include "storage.h"

#include <EEPROM.h>

namespace {

template<typename T>
void eepromWrite32(int offset, T value) {
  uint32_t v = static_cast<uint32_t>(value);
  for (int i = 0; i < 4; i++) {
    EEPROM.write(offset + i, (v >> (24 - i * 8)) & 0xFF);
  }
}

template<typename T>
T eepromRead32(int offset) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    v = (v << 8) | EEPROM.read(offset + i);
  }
  return static_cast<T>(v);
}

}  // namespace

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
  eepromWrite32(EEPROM_WAKE_COUNTER_OFFSET, counter);
  EEPROM.commit();
}

uint32_t loadWakeCounterFromEEPROM() {
  return eepromRead32<uint32_t>(EEPROM_WAKE_COUNTER_OFFSET);
}

String loadChecksumFromEEPROM() {
  char buffer[EEPROM_CHECKSUM_MAX_LEN + 1];
  int len = 0;
  for (int i = 0; i < EEPROM_CHECKSUM_MAX_LEN; i++) {
    char value = EEPROM.read(i);
    if (value == '\0') break;
    buffer[len++] = value;
  }
  buffer[len] = '\0';
  return String(buffer);
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
  eepromWrite32(EEPROM_REFRESH_INTERVAL_OFFSET, intervalSeconds);
  EEPROM.commit();
}

uint32_t loadRefreshIntervalFromEEPROM() {
  return eepromRead32<uint32_t>(EEPROM_REFRESH_INTERVAL_OFFSET);
}

void saveElapsedFullFetchSecondsToEEPROM(uint32_t elapsedSeconds) {
  eepromWrite32(EEPROM_FULL_FETCH_ELAPSED_OFFSET, elapsedSeconds);
  EEPROM.commit();
}

uint32_t loadElapsedFullFetchSecondsFromEEPROM() {
  return eepromRead32<uint32_t>(EEPROM_FULL_FETCH_ELAPSED_OFFSET);
}
