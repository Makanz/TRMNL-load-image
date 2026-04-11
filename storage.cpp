#include "storage.h"

#include <EEPROM.h>

namespace {

bool writeByteIfChanged(int address, uint8_t value) {
  if (EEPROM.read(address) == value) {
    return false;
  }

  EEPROM.write(address, value);
  return true;
}

bool writeUint32ToEEPROM(int offset, uint32_t value) {
  bool dirty = false;
  dirty = writeByteIfChanged(offset, (value >> 24) & 0xFF) || dirty;
  dirty = writeByteIfChanged(offset + 1, (value >> 16) & 0xFF) || dirty;
  dirty = writeByteIfChanged(offset + 2, (value >> 8) & 0xFF) || dirty;
  dirty = writeByteIfChanged(offset + 3, value & 0xFF) || dirty;
  return dirty;
}

uint32_t readUint32FromEEPROM(int offset) {
  return ((uint32_t)EEPROM.read(offset) << 24) |
         ((uint32_t)EEPROM.read(offset + 1) << 16) |
         ((uint32_t)EEPROM.read(offset + 2) << 8) |
         (uint32_t)EEPROM.read(offset + 3);
}

bool writeChecksumBytes(const String& checksum) {
  bool dirty = false;
  size_t len = checksum.length();
  if (len > EEPROM_CHECKSUM_MAX_LEN) {
    len = EEPROM_CHECKSUM_MAX_LEN;
  }

  for (size_t i = 0; i < EEPROM_CHECKSUM_MAX_LEN; i++) {
    uint8_t value = (i < len) ? static_cast<uint8_t>(checksum[i]) : '\0';
    dirty = writeByteIfChanged(static_cast<int>(i), value) || dirty;
  }

  return dirty;
}

void commitIfNeeded(bool shouldCommit, bool dirty) {
  if (shouldCommit && dirty) {
    EEPROM.commit();
  }
}

}  // namespace

void clearChecksumInEEPROM() {
  saveChecksumToEEPROM("");
}

void saveChecksumToEEPROM(const String& checksum, bool commit) {
  commitIfNeeded(commit, writeChecksumBytes(checksum));
}

void saveWakeCounterToEEPROM(uint32_t counter, bool commit) {
  commitIfNeeded(commit, writeUint32ToEEPROM(EEPROM_WAKE_COUNTER_OFFSET, counter));
}

uint32_t loadWakeCounterFromEEPROM() {
  return readUint32FromEEPROM(EEPROM_WAKE_COUNTER_OFFSET);
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

void savePersistedState(const FirmwareState& state) {
  bool dirty = false;
  dirty = writeChecksumBytes(state.storedChecksum) || dirty;
  dirty = writeUint32ToEEPROM(EEPROM_WAKE_COUNTER_OFFSET, state.wakeCounter) || dirty;
  dirty = writeUint32ToEEPROM(EEPROM_REFRESH_INTERVAL_OFFSET, state.refreshIntervalSeconds) || dirty;
  dirty = writeUint32ToEEPROM(EEPROM_FULL_FETCH_ELAPSED_OFFSET, state.elapsedFullFetchSeconds) || dirty;
  commitIfNeeded(true, dirty);
}

void clearPersistedImageState(FirmwareState& state) {
  state.storedChecksum = "";
  state.previousChecksum = "";
  state.wakeCounter = 0;
  state.refreshIntervalSeconds = 0;
  state.elapsedFullFetchSeconds = 0;
  savePersistedState(state);
}

void saveRefreshIntervalToEEPROM(uint32_t intervalSeconds, bool commit) {
  commitIfNeeded(commit, writeUint32ToEEPROM(EEPROM_REFRESH_INTERVAL_OFFSET, intervalSeconds));
}

uint32_t loadRefreshIntervalFromEEPROM() {
  return readUint32FromEEPROM(EEPROM_REFRESH_INTERVAL_OFFSET);
}

void saveElapsedFullFetchSecondsToEEPROM(uint32_t elapsedSeconds, bool commit) {
  commitIfNeeded(commit, writeUint32ToEEPROM(EEPROM_FULL_FETCH_ELAPSED_OFFSET, elapsedSeconds));
}

uint32_t loadElapsedFullFetchSecondsFromEEPROM() {
  return readUint32FromEEPROM(EEPROM_FULL_FETCH_ELAPSED_OFFSET);
}
