#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <math.h>

// =============================================================================
//  THRUST RIG GROUND STATION (RECEIVER)
//  - Wiring/pin defines kept unchanged (CE:D9, CSN:D8, SCK:D13, MOSI:D11, MISO:D12)
//  - Binary TelemetryPacket must match transmitter
// =============================================================================

const int LED_PIN = 13;   // D13 is also SPI SCK — unchanged

RF24 radio(9, 8);     // CE, CSN
const byte address[6] = "THRST";

// MUST be byte-for-byte identical to the transmitter's struct.
struct TelemetryPacket {
  unsigned long time_ms;
  long rawADC;
  float forceN;
};

unsigned long packetCount  = 0;
unsigned long lastRxMillis  = 0;
const unsigned long LINK_TIMEOUT_MS = 2000;  // warn if no packet for this long
bool linkAlive = false;

unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_MS = 5000; // print heartbeat every 5 seconds

void setup() {
  Serial.begin(115200);
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

  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_250KBPS);
  radio.setChannel(108);
  radio.setRetries(3, 5);
  radio.setPayloadSize(sizeof(TelemetryPacket));
  radio.openReadingPipe(1, address);
  radio.startListening();
  Serial.println(F("OK"));

  Serial.println();
  Serial.println(F("Time_ms,Raw_ADC,Force_N"));   // CSV header

  Serial.println(F("[CMD] Available: S=Status, R=Reset counts, H=Help"));
}

void loop() {
  // handle incoming radio packets
  if (radio.available()) {
    TelemetryPacket packet;
    radio.read(&packet, sizeof(packet));

    // basic sanity checks
    bool valid = true;
    if (!isfinite(packet.forceN)) valid = false;
    if (fabs(packet.forceN) > 1e6f) valid = false;    // absurd force
    if (packet.rawADC == 0L && packet.time_ms == 0UL) valid = false; // likely corrupted

    if (valid) {
      packetCount++;
      lastRxMillis = millis();
      if (!linkAlive) {
        linkAlive = true;
        Serial.println(F("[NRF]   Link UP."));
      }
      printPacketCSV(packet);
    } else {
      Serial.println(F("[WARN] Received packet failed sanity checks; ignored."));
    }
  }

  // Link-loss watchdog
  if (linkAlive && (millis() - lastRxMillis > LINK_TIMEOUT_MS)) {
    linkAlive = false;
    Serial.print(F("[NRF]   Link LOST after "));
    Serial.print(packetCount);
    Serial.println(F(" packets."));
  }

  // LED on while link is alive
  digitalWrite(LED_PIN, linkAlive ? HIGH : LOW);

  // Periodic heartbeat/status
  if (millis() - lastHeartbeat > HEARTBEAT_MS) {
    lastHeartbeat = millis();
    Serial.print(F("[HB] packets="));
    Serial.print(packetCount);
    Serial.print(F(" link="));
    Serial.println(linkAlive ? F("UP") : F("DOWN"));
  }

  // Serial command handling (non-blocking)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) return;
    char c = cmd.charAt(0);
    if (c == 'S' || c == 's') {
      Serial.println(F("[STATUS]"));
      Serial.print(F(" packets_received="));
      Serial.println(packetCount);
      Serial.print(F(" linkAlive="));
      Serial.println(linkAlive ? F("true") : F("false"));
      unsigned long age = (lastRxMillis == 0) ? 0 : (millis() - lastRxMillis);
      Serial.print(F(" last_packet_age_ms="));
      Serial.println(age);
    } else if (c == 'R' || c == 'r') {
      packetCount = 0;
      Serial.println(F("[CMD] Counters reset."));
    } else if (c == 'H' || c == 'h') {
      Serial.println(F("[HELP] Commands:"));
      Serial.println(F(" S - print status"));
      Serial.println(F(" R - reset packet counters"));
      Serial.println(F(" H - help"));
    } else {
      Serial.println(F("[CMD] Unknown command. Send 'H' for help."));
    }
  }
}

void printPacketCSV(const TelemetryPacket &p) {
  char fBuf[12];
  dtostrf(p.forceN, 0, 3, fBuf);

  Serial.print(p.time_ms);
  Serial.print(',');
  Serial.print(p.rawADC);
  Serial.print(',');
  Serial.println(fBuf);
}
