#include <HX711.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <EEPROM.h>

// =============================================================================
//  ROCKET STATIC MOTOR THRUST TEST RIG — PATCHED TRANSMITTER
//  - Keeps original wiring/pin defines unchanged
//  - Adds EEPROM persistence for calibration/tare, non-blocking settle,
//    tare timeout, serial commands (C/W/L/S/T), and safer Serial handling.
// =============================================================================

// Calibration default (will be overwritten from EEPROM if saved)
float calibrationFactor = 24374.0f;  // <-- your calibrated value

// EEPROM layout
const uint8_t EEPROM_MAGIC = 0xA5;
const int EEPROM_MAGIC_ADDR = 0;
const int EEPROM_CAL_ADDR   = EEPROM_MAGIC_ADDR + 1; // float (4 bytes)
const int EEPROM_TARE_ADDR  = EEPROM_CAL_ADDR + sizeof(float); // long (4 bytes)

// =============================================================================
//  PIN DEFINITIONS (UNCHANGED)
// =============================================================================
const int HX711_SCK_PIN  = 4;
const int HX711_DOUT_PIN = 5;

const int LED_PIN        = 13;   // NOTE: D13 is also SPI SCK. Left unchanged.

// =============================================================================
//  SAMPLING CONFIGURATION (unchanged)
const unsigned long SAMPLE_INTERVAL_US = 100000;  // 100 ms = 10 Hz

// =============================================================================
//  TARE / FILTER CONFIGURATION
const int TARE_SAMPLES   = 100;
const unsigned long TARE_TIMEOUT_MS = 10000UL; // max 10 s to collect tare samples
const unsigned long SETTLE_TIME_MS  = 30000UL; // original settle time
const int FILTER_SIZE    = 5;

// =============================================================================
//  GLOBALS
// =============================================================================
HX711 scale;

long   tareOffset    = 0;
unsigned long startTime   = 0;
unsigned long lastSample  = 0;

long filterBuf[8];
int  filterIdx = 0;
bool filterFull = false;

// ---- Radio ----
RF24 radio(9, 10);     // CE, CSN (unchanged)
const byte address[6] = "THRST";

struct TelemetryPacket {
  unsigned long time_ms;
  long rawADC;
  float forceN;
}; // must match receiver layout exactly

unsigned long txFailCount = 0;   // telemetry packets that failed to ACK
unsigned long sampleCount = 0;

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================
void initHX711();
void initRadio();
bool performTare();                       // returns success
long applyFilter(long newVal);
float rawToForce(long rawValue);
void printSerialData(float timeMs, long rawADC, float forceN);
void checkSerialCommands();
void haltWithError(const char* msg, bool blinkLED);
void saveConfigToEEPROM();
bool loadConfigFromEEPROM();

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  // NOTE: removed blocking "while (!Serial) {}" to avoid hanging on non-USB boards

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println(F("=============================================="));
  Serial.println(F("  ROCKET STATIC MOTOR THRUST TEST RIG (PATCH)"));
  Serial.println(F("=============================================="));
  Serial.println();

  // Load persisted calibration/tare if present
  if (loadConfigFromEEPROM()) {
    Serial.println(F("[EEPROM] Calibration and tare loaded"));
  } else {
    Serial.println(F("[EEPROM] No saved config found; using built-in defaults"));
  }
  Serial.print(F("[CAL] calibrationFactor="));
  Serial.println(calibrationFactor, 6);

  initHX711();
  initRadio();

  // Non-blocking settle: allow Serial input while waiting
  Serial.print(F("[INIT]  Settling ("));
  Serial.print(SETTLE_TIME_MS);
  Serial.println(F(" ms)... (press 'T' to tare early)"));

  unsigned long settleStart = millis();
  while (millis() - settleStart < SETTLE_TIME_MS) {
    checkSerialCommands();   // lets user interact during settling
    delay(50);
  }

  // Perform tare (with timeout)
  if (!performTare()) {
    Serial.println(F("[WARN] Tare timed out — continuing with best-effort offset"));
  }

  // Seed the filter buffer with the tare offset to avoid step response
  for (int i = 0; i < FILTER_SIZE; i++) {
    filterBuf[i] = tareOffset;
  }
  filterFull = true;
  filterIdx = 0;

  startTime  = micros();
  lastSample = startTime;

  Serial.println();
  Serial.println(F("Time_ms\t\tRaw_ADC\t\tForce_N"));
  Serial.println(F("----------------------------------------------"));
  Serial.println(F("[READY] Logging + transmitting started."));
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  checkSerialCommands();

  unsigned long now = micros();
  if ((now - lastSample) < SAMPLE_INTERVAL_US) return;
  lastSample += SAMPLE_INTERVAL_US;   // keep schedule aligned

  if (!scale.is_ready()) return;

  long  rawADC   = scale.read();
  long  filtered = applyFilter(rawADC);
  float timeMs   = (float)(now - startTime) / 1000.0f;
  float forceN   = rawToForce(filtered);

  TelemetryPacket packet;
  packet.time_ms = (now - startTime) / 1000UL;
  packet.rawADC  = filtered;
  packet.forceN  = forceN;

  if (!radio.write(&packet, sizeof(packet))) {
    txFailCount++;          // keep counting; avoid heavy handling in hot path
  }

  printSerialData(timeMs, filtered, forceN);

  sampleCount++;
  if (sampleCount % 50 == 0) {
    Serial.print(F("[NRF]   tx_fails="));
    Serial.print(txFailCount);
    Serial.print(F(" / "));
    Serial.print(sampleCount);
    Serial.println(F(" sent"));
  }
}

// =============================================================================
//  initHX711
// =============================================================================
void initHX711() {
  Serial.print(F("[HX711] Initializing... "));
  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  unsigned long t0 = millis();
  while (!scale.is_ready()) {
    if (millis() - t0 > 3000) {
      haltWithError("HX711 not found. Check wiring and power.", false);
    }
    delay(10);
  }
  scale.set_gain(128);
  Serial.println(F("OK"));
}

// =============================================================================
//  initRadio
// =============================================================================
void initRadio() {
  Serial.print(F("[NRF]   Initializing... "));
  if (!radio.begin()) {
    haltWithError("NRF24L01 not found. Check 3.3V supply, CE=D9, CSN=D8, SPI wiring.", false);
  }

  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(108);
  radio.setRetries(3, 5);
  radio.setPayloadSize(sizeof(TelemetryPacket));
  radio.openWritingPipe(address);
  radio.stopListening();

  Serial.println(F("OK"));
}

// =============================================================================
//  performTare  (now with timeout and best-effort behavior)
// =============================================================================
bool performTare() {
  Serial.print(F("[HX711] Taring ("));
  Serial.print(TARE_SAMPLES);
  Serial.print(F(" samples)... "));

  long long sum = 0;
  int good = 0;
  unsigned long t0 = millis();

  while (good < TARE_SAMPLES) {
    if (scale.is_ready()) {
      sum += scale.read();
      good++;
    }
    if (millis() - t0 > TARE_TIMEOUT_MS) {
      break; // timeout
    }
  }

  if (good == 0) {
    Serial.println(F("FAILED (no samples)"));
    // keep existing tareOffset (likely zero) and return failure
    return false;
  }

  tareOffset = (long)(sum / good);
  Serial.print(F("OK  offset="));
  Serial.println(tareOffset);
  return (good >= TARE_SAMPLES);
}

// =============================================================================
//  applyFilter
// =============================================================================
long applyFilter(long newVal) {
  filterBuf[filterIdx] = newVal;
  filterIdx = (filterIdx + 1) % FILTER_SIZE;
  if (filterIdx == 0) filterFull = true;

  int count = filterFull ? FILTER_SIZE : filterIdx;
  long long sum = 0;
  for (int i = 0; i < count; i++) {
    sum += filterBuf[i];
  }
  return (long)(sum / count);
}

// =============================================================================
//  rawToForce
// =============================================================================
float rawToForce(long rawValue) {
  // returns in 'units' determined by calibrationFactor
  // if calibrationFactor maps counts -> Newtons directly, this is Newtons.
  return -(float)(rawValue - tareOffset) / calibrationFactor;
}

// =============================================================================
//  printSerialData
// =============================================================================
void printSerialData(float timeMs, long rawADC, float forceN) {
  char tBuf[12];
  char fBuf[12];
  dtostrf(timeMs, 8, 1, tBuf);
  dtostrf(forceN, 8, 3, fBuf);

  Serial.print(tBuf);
  Serial.print(F(" ms\t"));
  Serial.print(rawADC);
  Serial.print(F("\t"));
  Serial.print(fBuf);
  Serial.println(F(" N"));
}

// =============================================================================
//  checkSerialCommands
//  Supported commands (newline-terminated):
//    T          - re-tare (existing behavior)
//    C <float>  - set calibrationFactor (example: "C 23660.0")
//    W          - write current calibrationFactor and tareOffset to EEPROM
//    L          - load calibrationFactor and tareOffset from EEPROM
//    S          - print status (calibration, tare, sample counts)
// =============================================================================
void checkSerialCommands() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char cmd = line.charAt(0);
  if (cmd == 'T' || cmd == 't') {
    Serial.println(F("\n[CMD] Re-taring... ensure load cell is UNLOADED."));
    delay(250);
    if (!performTare()) {
      Serial.println(F("[CMD] Tare failed (timeout)."));
    } else {
      for (int i = 0; i < FILTER_SIZE; i++) filterBuf[i] = tareOffset;
      filterIdx  = 0;
      filterFull = true;
      Serial.println(F("[CMD] Tare complete."));
    }
    return;
  }

  if (cmd == 'C' || cmd == 'c') {
    // Expect "C <value>"
    int sp = line.indexOf(' ');
    if (sp > 0) {
      String token = line.substring(sp + 1);
      token.trim();
      float v = token.toFloat();
      if (v > 0.0f) {
        calibrationFactor = v;
        Serial.print(F("[CMD] calibrationFactor set to "));
        Serial.println(calibrationFactor, 6);
      } else {
        Serial.println(F("[CMD] Invalid calibration value"));
      }
    } else {
      Serial.println(F("[CMD] Usage: C <float>"));
    }
    return;
  }

  if (cmd == 'W' || cmd == 'w') {
    saveConfigToEEPROM();
    Serial.println(F("[CMD] Config written to EEPROM."));
    return;
  }

  if (cmd == 'L' || cmd == 'l') {
    if (loadConfigFromEEPROM()) {
      Serial.println(F("[CMD] Config loaded from EEPROM."));
      Serial.print(F("[CAL] calibrationFactor="));
      Serial.println(calibrationFactor, 6);
      Serial.print(F("[TARE] tareOffset="));
      Serial.println(tareOffset);
    } else {
      Serial.println(F("[CMD] No valid config in EEPROM."));
    }
    return;
  }

  if (cmd == 'S' || cmd == 's') {
    Serial.println(F("[STATUS]"));
    Serial.print(F(" calibrationFactor="));
    Serial.println(calibrationFactor, 6);
    Serial.print(F(" tareOffset="));
    Serial.println(tareOffset);
    Serial.print(F(" txFails="));
    Serial.print(txFailCount);
    Serial.print(F(" samples="));
    Serial.println(sampleCount);
    return;
  }

  Serial.println(F("[CMD] Unknown command. Supported: T, C <val>, W, L, S"));
}

// =============================================================================
//  EEPROM helpers
// =============================================================================
void saveConfigToEEPROM() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  EEPROM.put(EEPROM_CAL_ADDR, calibrationFactor);
  EEPROM.put(EEPROM_TARE_ADDR, tareOffset);
}

bool loadConfigFromEEPROM() {
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC) return false;
  float cal = 0;
  long tare = 0;
  EEPROM.get(EEPROM_CAL_ADDR, cal);
  EEPROM.get(EEPROM_TARE_ADDR, tare);
  // Basic sanity checks
  if (isnan(cal) || cal <= 0.0f || cal > 1e8f) return false;
  calibrationFactor = cal;
  tareOffset = tare;
  return true;
}

// =============================================================================
//  haltWithError
// =============================================================================
void haltWithError(const char* msg, bool blinkLED) {
  Serial.println();
  Serial.println(F("!!! FATAL ERROR !!!"));
  Serial.println(msg);
  Serial.println(F("Power cycle to restart."));
  while (true) {
    if (blinkLED) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    } else {
      delay(1000);
    }
  }
}
