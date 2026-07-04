// =============================================================================
//  ROCKET STATIC MOTOR THRUST TEST RIG — GROUND STATION / RECEIVER
//  Arduino Uno / Nano (ATmega328P)
// =============================================================================
//
//  Hardware:
//    - Arduino Uno or Nano (ATmega328P)
//    - NRF24L01 2.4 GHz radio (SPI)
//
//  Wiring:
//    NRF24L01 -> CE:D9, CSN:D8, SCK:D13, MOSI:D11, MISO:D12, VCC:3.3V, GND:GND
//
//  *** NRF24L01 POWER ***
//    - VCC = 3.3V ONLY (logic pins are 5V-tolerant).
//    - 10uF (+ 0.1uF) cap directly across the module's VCC/GND pins.
//
//  MUST MATCH THE TRANSMITTER:
//    channel 108  |  250 kbps  |  address "THRST"  |  identical TelemetryPacket
//
//  Serial output is CSV:  Time_ms,Raw_ADC,Force_N   (115200 baud)
//  Copy/paste the serial log straight into a spreadsheet.
//
//  Libraries:
//    - RF24 by TMRh20
//    - SPI  (built-in)
//
// =============================================================================

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================
const int LED_PIN = 13;   // NOTE: D13 is also SPI SCK — flickers during RX, harmless.

// =============================================================================
//  RADIO
// =============================================================================
RF24 radio(9, 8);     // CE, CSN
const byte address[6] = "THRST";

// MUST be byte-for-byte identical to the transmitter's struct.
struct TelemetryPacket {
  unsigned long time_ms;
  long rawADC;
  float forceN;
};

// =============================================================================
//  LINK-HEALTH TRACKING
// =============================================================================
unsigned long packetCount  = 0;
unsigned long lastRxMillis  = 0;
const unsigned long LINK_TIMEOUT_MS = 2000;  // warn if no packet for this long
bool linkAlive = false;

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println(F("=============================================="));
  Serial.println(F("  THRUST RIG GROUND STATION (RECEIVER)"));
  Serial.println(F("=============================================="));

  Serial.print(F("[NRF]   Initializing... "));
  if (!radio.begin()) {
    Serial.println(F("FAILED"));
    Serial.println(F("!!! NRF24L01 not found. Check 3.3V supply, CE=D9, CSN=D8, SPI wiring."));
    while (true) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(200);
    }
  }

  radio.setPALevel(RF24_PA_LOW);          // match-ish; PA level need not equal TX
  radio.setDataRate(RF24_250KBPS);        // MUST match transmitter
  radio.setChannel(108);                  // MUST match transmitter
  radio.setRetries(3, 5);                 // affects auto-ACK timing; fine as-is
  radio.setPayloadSize(sizeof(TelemetryPacket));
  radio.openReadingPipe(1, address);      // MUST match transmitter's writing pipe
  radio.startListening();                 // receiver mode
  Serial.println(F("OK"));

  Serial.println();
  Serial.println(F("Time_ms,Raw_ADC,Force_N"));   // CSV header
}

// =============================================================================
//  LOOP
// =============================================================================
void loop() {
  if (radio.available()) {
    TelemetryPacket packet;
    radio.read(&packet, sizeof(packet));

    packetCount++;
    lastRxMillis = millis();
    if (!linkAlive) {
      linkAlive = true;
      Serial.println(F("[NRF]   Link UP."));
    }

    printPacketCSV(packet);
  }

  // ---- Link-loss watchdog ----
  if (linkAlive && (millis() - lastRxMillis > LINK_TIMEOUT_MS)) {
    linkAlive = false;
    Serial.print(F("[NRF]   Link LOST after "));
    Serial.print(packetCount);
    Serial.println(F(" packets."));
  }

  // LED on while link is alive
  digitalWrite(LED_PIN, linkAlive ? HIGH : LOW);
}

// =============================================================================
//  printPacketCSV  — one CSV row per received packet
// =============================================================================
void printPacketCSV(const TelemetryPacket &p) {
  char fBuf[12];
  dtostrf(p.forceN, 0, 3, fBuf);

  Serial.print(p.time_ms);
  Serial.print(',');
  Serial.print(p.rawADC);
  Serial.print(',');
  Serial.println(fBuf);
}
