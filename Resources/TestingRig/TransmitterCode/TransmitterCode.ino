// =============================================================================
//  ROCKET STATIC MOTOR THRUST TEST RIG
//  Arduino Uno / Nano (ATmega328P)
// =============================================================================
//
//  Hardware:
//    - Arduino Uno or Nano (ATmega328P)
//    - HX711 Load Cell Amplifier
//    - 4-Wire Load Cell
//    - NRF24L01 2.4 GHz radio (SPI)
//    - 5V regulated supply  (NRF24L01 must be fed 3.3V — see note below!)
//
//  Wiring:
//    HX711     -> DOUT:D3, SCK:D2, VCC:5V, GND:GND          (bit-banged, NOT SPI)
//    NRF24L01  -> CE:D9, CSN:D8, SCK:D13, MOSI:D11, MISO:D12, VCC:3.3V, GND:GND
//    Load Cell -> Red:E+, Black:E-, Green:A+, White:A-
//
//  *** NRF24L01 POWER — READ THIS ***
//    - VCC = 3.3V ONLY. 5V will destroy the module. Logic pins ARE 5V-tolerant.
//    - Solder a 10uF cap (+ 0.1uF) directly across the module's VCC/GND pins.
//      The radio draws sharp current spikes on every transmit; without local
//      decoupling those spikes sag the rail and corrupt HX711 readings or reset
//      the board. This is the #1 cause of "noise that appears when the radio
//      is on." Keep PA level LOW on the bench unless decoupling is solid.
//
//  CSV Output:  Time_ms, Raw_ADC, Force_N
//
//  Serial Commands (send via Serial Monitor):
//    T  -> Re-tare (zero the load cell)
//
//  Libraries:
//    - HX711 by Bogdan Necula (v0.7.5+)
//    - RF24  by TMRh20
//    - SPI   (built-in)
//
// =============================================================================

#include <HX711.h>
#include <SPI.h>

#include <nRF24L01.h>
#include <RF24.h>

// =============================================================================
//  CALIBRATION
// =============================================================================
float calibrationFactor = 23660.0;  // <-- your calibrated value

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================
const int HX711_SCK_PIN  = 2;
const int HX711_DOUT_PIN = 3;

const int LED_PIN        = 13;   // NOTE: D13 is also SPI SCK. The LED will flicker
                                 // during radio transfers — harmless here since
                                 // it's only used for fatal-error blink.

// =============================================================================
//  SAMPLING CONFIGURATION
// =============================================================================
//  10 Hz -> 100000 us    80 Hz -> 12500 us
//  If you move to 80 Hz, drop setRetries() further (see radio init) or switch to
//  a non-blocking write, otherwise a slow ACK can eat the sample budget.
const unsigned long SAMPLE_INTERVAL_US = 100000;  // 100 ms = 10 Hz

// =============================================================================
//  TARE / FILTER CONFIGURATION
// =============================================================================
const int TARE_SAMPLES   = 100;
const int SETTLE_TIME_MS = 30000;
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
RF24 radio(9, 8);     // CE, CSN
const byte address[6] = "THRST";

struct TelemetryPacket {
  unsigned long time_ms;
  long rawADC;
  float forceN;
};                    // 12 bytes on AVR. Receiver must use an IDENTICAL struct.

unsigned long txFailCount = 0;   // telemetry packets that failed to ACK
unsigned long sampleCount = 0;

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================
void initHX711();
void initRadio();
void performTare();
long applyFilter(long newVal);
float rawToForce(long rawValue);
void printSerialData(float timeMs, long rawADC, float forceN);
void checkSerialCommands();
void haltWithError(const char* msg, bool blinkLED);

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println(F("=============================================="));
  Serial.println(F("  ROCKET STATIC MOTOR THRUST TEST RIG"));
  Serial.println(F("=============================================="));
  Serial.println();

  initHX711();
  initRadio();

  // Let the load cell and amplifier settle before zeroing
  Serial.print(F("[INIT]  Settling ("));
  Serial.print(SETTLE_TIME_MS);
  Serial.println(F(" ms)..."));
  delay(SETTLE_TIME_MS);

  performTare();

  // Reset the filter buffer to the tare offset so the MA doesn't
  // drag through a step response on the first few samples.
  for (int i = 0; i < FILTER_SIZE; i++) {
    filterBuf[i] = tareOffset;
  }
  filterFull = true;

  startTime  = micros();
  lastSample = startTime;

  Serial.println();
  Serial.println(F("Time_ms\t\tRaw_ADC\t\tForce_N"));
  Serial.println(F("----------------------------------------------"));
  Serial.println(F("[READY] Logging + transmitting started."));
}

void loop() {
  checkSerialCommands();

  unsigned long now = micros();
  if ((now - lastSample) < SAMPLE_INTERVAL_US) return;
  lastSample += SAMPLE_INTERVAL_US;   // self-correcting schedule: even if the
                                      // radio stalls a loop, the next sample
                                      // fires at the correct absolute time.

  if (!scale.is_ready()) return;

  // ---- Capture the data point FIRST. Everything below (radio, serial) happens
  //      after the value + timestamp are locked in, so a slow or failed radio
  //      write cannot shift WHEN the reading was taken. ----
  long  rawADC   = scale.read();
  long  filtered = applyFilter(rawADC);
  float timeMs   = (float)(now - startTime) / 1000.0f;
  float forceN   = rawToForce(filtered);

  // ---- Transmit telemetry ----
  TelemetryPacket packet;
  packet.time_ms = (now - startTime) / 1000UL;
  packet.rawADC  = filtered;
  packet.forceN  = forceN;

  if (!radio.write(&packet, sizeof(packet))) {
    txFailCount++;          // just count; no printing in the hot path
  }

  printSerialData(timeMs, filtered, forceN);

  // ---- Periodic telemetry-health line (every 50 samples ~ 5 s @ 10 Hz) ----
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
      haltWithError("HX711 not found. Check wiring (DOUT->D3, SCK->D2).", false);
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

  radio.setPALevel(RF24_PA_LOW);          // LOW = least current draw / most stable
                                          // on the bench. Raise to HIGH only with
                                          // good decoupling on the module.
  radio.setDataRate(RF24_250KBPS);        // best range & link margin
  radio.setChannel(108);                  // 2.508 GHz, above most WiFi
  radio.setRetries(3, 5);                 // (4*250us delay) x 5 retries:
                                          // bounded ~9 ms worst case, well inside
                                          // the 100 ms sample window.
  radio.setPayloadSize(sizeof(TelemetryPacket));
  radio.openWritingPipe(address);
  radio.stopListening();                  // transmitter mode

  Serial.println(F("OK"));
}

// =============================================================================
//  performTare
// =============================================================================
void performTare() {
  Serial.print(F("[HX711] Taring ("));
  Serial.print(TARE_SAMPLES);
  Serial.print(F(" samples)... "));

  long long sum = 0;
  int good = 0;

  while (good < TARE_SAMPLES) {
    if (scale.is_ready()) {
      sum += scale.read();
      good++;
    }
  }
  tareOffset = (long)(sum / TARE_SAMPLES);

  Serial.print(F("OK  offset="));
  Serial.println(tareOffset);
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
  return (float)(rawValue - tareOffset) / calibrationFactor;
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
//  checkSerialCommands  — 'T' to re-tare
// =============================================================================
void checkSerialCommands() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'T' || c == 't') {
      Serial.println(F("\n[CMD] Re-taring... ensure load cell is UNLOADED."));
      delay(1000);
      performTare();
      for (int i = 0; i < FILTER_SIZE; i++) {
        filterBuf[i] = tareOffset;
      }
      filterIdx  = 0;
      filterFull = true;
      Serial.println(F("[CMD] Tare complete. Logging resumed.\n"));
    }
  }
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
    }
  }
}