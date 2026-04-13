# Testing Framework Design

## Recommended Approach

**PlatformIO with Unity testing framework** is the best practice for this ESP32/Arduino project.

## Why PlatformIO + Unity

| Criteria | PlatformIO + Unity | ArduinoUnit |
|----------|-------------------|------------|
| Hardware mocking | Excellent native support | Limited |
| Host testing | Native support | Not supported |
| ESP32 toolchain | Built-in | Manual setup |
| CI/CD integration | Excellent | Basic |
| Test reporting | HTML, JSON, JUnit | Serial output |
| Library management | Auto-resolve deps | Manual |

## Architecture

```
project/
├── platformio.ini           # Build config + test env
├── test/
│   ├── host/               # Host-based unit tests (no hardware)
│   │   ├── test_storage.cpp
│   │   └── test_firmware_state.cpp
│   └── native/             # Hardware tests (ESP32)
│       ├── test_storage_hw.cpp
│       └── test_api.cpp
├── mock/
│   ├── mock_EEPROM.h       # Mock EEPROM for host testing
│   ├── mock_WiFi.h         # Mock WiFi for host testing
│   └── mock_EPaper.h       # Mock display for host testing
└── src/                   # Existing source
```

## Implementation Steps

### Phase 1: Infrastructure
1. Add `platformio.ini` with test_environment configuration
2. Create test harness with Unity + mocks
3. Verify tests compile and run on host

### Phase 2: Unit Tests
1. `firmware_state.h` - validation functions, constants
2. `storage.cpp` - EEPROM read/write logic
3. `config.h` - interval validation

### Phase 3: Integration Tests
1. API response parsing (mock HTTP)
2. BMP header parsing
3. WiFi manager connection logic

## Testable Code Separation

The code has good separation already. Key test targets:

| Module | Test Approach | Mock Strategy |
|--------|------------|-------------|
| `firmware_state.h` | Pure functions | No mocks needed |
| `storage.cpp` | Host tests | mock_EEPROM |
| `wifi_manager.cpp` | Host tests | mock_WiFi |
| `image_api.cpp` | Host tests | mock_HTTP |
| `bmp_decode.cpp` | Host tests | mock_Stream |
| `display_ui.cpp` | Host tests | mock_EPaper |

## Recommendations

1. **Start with host-based tests** - faster iteration, no hardware needed
2. **Mock hardware dependencies** - WiFi, EEPROM, display via header injection
3. **Test business logic first** - validation, parsing, state management
4. **Use #ifdef for platform differences** - separate host/device code paths

## Success Criteria

- [ ] Tests run on host machine without ESP32 hardware
- [ ] Core validation functions tested
- [ ] EEPROM storage logic tested
- [ ] CI/CD pipeline configured
- [ ] Test report generation working