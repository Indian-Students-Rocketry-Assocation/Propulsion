// ===== Static Motor Test Stand - ADS1232 + ESP32 =====
// ADS1232: SCLK -> GPIO32, DOUT -> GPIO34, PDWN -> 3.3V, SPEED -> GND (10 SPS)
// Relay:   IN -> GPIO23
// Wi-Fi:   connect to "ThrustStand" (password thrust123), open http://192.168.4.1

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ---------- Pins ----------
const int PIN_SCLK  = 32;
const int PIN_DOUT  = 34;
const int PIN_RELAY = 23;

// ---------- Settings ----------
const bool     RELAY_ACTIVE_HIGH       = false;   // false if your relay turns ON with LOW
const uint32_t PRE_IGNITION_MS         = 1000;   // baseline recorded before relay fires
const uint32_t IGNITER_ON_MS = 500;   // was 2000
const uint32_t MAX_TEST_MS             = 120000; // auto-end after 2 minutes
const int      MAX_SAMPLES             = 6000;
const int      AVG_SAMPLES             = 20;     // samples averaged for tare/calibration
const float    DEFAULT_COUNTS_PER_GRAM = 1.0;    // or calibrate from the web page
const char*    AP_SSID = "ThrustStand";
const char*    AP_PASS = "thrust123";            // at least 8 characters

const float G_TO_N = 0.00980665f;
const int RELAY_ON_LEVEL  = RELAY_ACTIVE_HIGH ? HIGH : LOW;
const int RELAY_OFF_LEVEL = RELAY_ACTIVE_HIGH ? LOW : HIGH;

// ---------- State ----------
struct Sample { uint32_t t_us; float grams; };
Sample* samples = nullptr;
volatile int sampleCount = 0;

enum TestState { IDLE, RUNNING, BURNED_OUT, DONE };
volatile TestState state = IDLE;

volatile uint32_t testStartUs = 0, ignitionUs = 0, burnoutUs = 0, endUs = 0;
volatile bool     ignitionFired = false, burnoutMarked = false;
volatile bool     firePending = false, relayOn = false;
volatile uint32_t fireAtMs = 0, relayOffAtMs = 0;

volatile long  tareOffset = 0;
volatile float countsPerGram = DEFAULT_COUNTS_PER_GRAM;
volatile float liveGrams = 0, peakGrams = 0;
volatile float burnTimeS = -1, impulseNs = 0;
volatile bool  adcOk = false;
volatile int   sps = 0;
volatile uint32_t lastSampleMs = 0;

volatile bool  tareRequest = false;
volatile float calRequest = 0;
volatile bool  calSavePending = false;

char statusMsg[80] = "Starting...";
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

WebServer server(80);
Preferences prefs;

void setMsg(const char* m) {
  strncpy(statusMsg, m, sizeof(statusMsg) - 1);
  statusMsg[sizeof(statusMsg) - 1] = 0;
}

void relaySet(bool on) {
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// ---------- ADS1232 ----------
long shiftIn24(int extraClocks) {
  long v = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_SCLK, HIGH);
    delayMicroseconds(2);
    v = (v << 1) | digitalRead(PIN_DOUT);
    digitalWrite(PIN_SCLK, LOW);
    delayMicroseconds(2);
  }
  for (int i = 0; i < extraClocks; i++) {
    digitalWrite(PIN_SCLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_SCLK, LOW);
    delayMicroseconds(2);
  }
  if (v & 0x800000) v |= 0xFF000000;
  return v;
}

bool readRawBlocking(long &out, int extraClocks) {
  uint32_t t0 = millis();
  while (digitalRead(PIN_DOUT) == HIGH) {
    if (millis() - t0 > 2000) return false;
    delay(1);
  }
  out = shiftIn24(extraClocks);
  return true;
}

// ---------- Test control ----------
void endTest() {
  bool doEnd = false;
  portENTER_CRITICAL(&mux);
  if (state == RUNNING || state == BURNED_OUT) { state = DONE; doEnd = true; }
  portEXIT_CRITICAL(&mux);
  if (!doEnd) return;

  firePending = false;
  relaySet(false);
  endUs = micros();

  burnTimeS = (ignitionFired && burnoutMarked) ? (burnoutUs - ignitionUs) / 1e6f : -1;

  // Total impulse (N·s) from ignition to burnout (or end of test)
  uint32_t a = ignitionFired ? (ignitionUs - testStartUs) : 0;
  uint32_t b = (burnoutMarked ? burnoutUs : endUs) - testStartUs;
  double imp = 0;
  for (int i = 1; i < sampleCount; i++) {
    uint32_t t0 = samples[i - 1].t_us, t1 = samples[i].t_us;
    if (t0 < a || t1 > b) continue;
    imp += 0.5 * (samples[i - 1].grams + samples[i].grams) * G_TO_N * (t1 - t0) / 1e6;
  }
  impulseNs = imp;
}

// ---------- ADC task (runs continuously) ----------
void adcTask(void*) {
  int skip = 10, accMode = 0, accN = 0;
  long long accSum = 0;
  float calG = 0;
  int spsCount = 0;
  uint32_t spsT = millis();

  for (;;) {
    uint32_t nowMs = millis();
    if (nowMs - spsT >= 1000) { sps = spsCount; spsCount = 0; spsT = nowMs; }
    if (nowMs - lastSampleMs > 1000) adcOk = false;

    if (digitalRead(PIN_DOUT) == HIGH) { vTaskDelay(1); continue; }

    long raw = shiftIn24(1);
    uint32_t tUs = micros();
    lastSampleMs = millis();
    adcOk = true;
    spsCount++;
    if (skip > 0) { skip--; continue; }

    // Tare / calibration
    if (accMode == 0) {
      if (tareRequest) {
        tareRequest = false; accMode = 1; accSum = 0; accN = 0;
        setMsg("Taring - keep stand still...");
      } else if (calRequest > 0) {
        calG = calRequest; calRequest = 0; accMode = 2; accSum = 0; accN = 0;
        setMsg("Calibrating - keep weight still...");
      }
    }
    if (accMode) {
      accSum += raw; accN++;
      if (accN >= AVG_SAMPLES) {
        long avg = accSum / accN;
        if (accMode == 1) {
          tareOffset = avg; liveGrams = 0;
          setMsg("Tare done");
        } else {
          long net = avg - tareOffset;
          if (labs(net) < 100) setMsg("Calibration failed - weight not detected");
          else { countsPerGram = net / calG; calSavePending = true; setMsg("Calibrated and saved"); }
        }
        accMode = 0;
      }
    }

    float g = (raw - tareOffset) / countsPerGram;
    liveGrams = 0.6f * liveGrams + 0.4f * g;

    if (state == RUNNING || state == BURNED_OUT) {
      if ((int32_t)(tUs - testStartUs) < 0) continue;
      int i = sampleCount;
      if (i < MAX_SAMPLES) {
        samples[i].t_us = tUs - testStartUs;
        samples[i].grams = g;
        sampleCount = i + 1;
        if (g > peakGrams) peakGrams = g;
      }
      if (sampleCount >= MAX_SAMPLES || (tUs - testStartUs) > MAX_TEST_MS * 1000UL) {
        endTest();
        setMsg("Test auto-ended (time/memory limit)");
      }
    }
  }
}

// ---------- Web page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Thrust Stand</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px;max-width:640px;margin:auto}
h1{font-size:20px;margin:0 0 8px}
.bar{display:flex;gap:8px;flex-wrap:wrap;font-size:13px;color:#aaa;margin-bottom:12px}
.pill{background:#222;padding:3px 8px;border-radius:10px}
.big{font-size:56px;font-weight:700;text-align:center;font-variant-numeric:tabular-nums}
.sub{text-align:center;color:#aaa;margin:6px 0}
canvas{width:100%;height:180px;background:#1a1a1a;border-radius:8px}
button,.btn{display:block;width:100%;padding:18px;margin:10px 0;font-size:20px;font-weight:700;border:0;border-radius:10px;color:#fff;text-align:center;text-decoration:none;box-sizing:border-box}
.red{background:#c62828}.orange{background:#ef6c00}.blue{background:#1565c0}.green{background:#2e7d32}
.grey{background:#444;font-size:15px;padding:10px}
.hide{display:none}
.row{display:flex;gap:8px;align-items:center}
input{flex:1;padding:10px;font-size:16px;background:#222;color:#eee;border:1px solid #444;border-radius:8px}
.card{background:#1a1a1a;border-radius:8px;padding:12px;margin:10px 0}
table{width:100%}td{padding:4px 0}td:last-child{text-align:right;font-weight:600}
</style></head><body>
<h1>Static Motor Test</h1>
<div class="bar"><span class="pill" id="st">-</span><span class="pill" id="conn">connecting</span>
<span class="pill" id="adc">ADC -</span><span class="pill" id="el"></span></div>
<div class="big" id="thrust">0.00 N</div>
<div class="sub" id="grams">0 g</div>
<canvas id="cv" width="600" height="180"></canvas>
<div class="sub" id="msg"></div>

<div id="pIdle">
  <button class="red" onclick="ignite()">IGNITE</button>
  <div class="card">
    <button class="grey" onclick="post('/tare')">Tare (motor mounted, no load)</button>
    <div class="row"><input id="calg" type="number" placeholder="Known weight (g)">
    <button class="grey" style="width:auto;margin:0" onclick="cal()">Calibrate</button></div>
    <div class="sub" id="cpg"></div>
  </div>
</div>
<div id="pRun" class="hide">
  <div class="sub" id="countdown"></div>
  <button class="orange" id="bBurn" onclick="post('/burnout')">BURN OUT</button>
  <button class="grey" onclick="if(confirm('Abort test? Relay will switch off.'))post('/end')">Abort</button>
</div>
<div id="pBurn" class="hide"><button class="blue" onclick="post('/end')">END TEST</button></div>
<div id="pDone" class="hide">
  <div class="card"><table>
    <tr><td>Peak thrust</td><td id="rPeak"></td></tr>
    <tr><td>Burn time</td><td id="rBurn"></td></tr>
    <tr><td>Total impulse</td><td id="rImp"></td></tr>
    <tr><td>Samples</td><td id="rN"></td></tr>
  </table></div>
  <a class="btn green" href="/thrust.csv" download="thrust.csv">Download Thrust CSV</a>
  <a class="btn green" href="/events.csv" download="events.csv">Download Events CSV</a>
  <button class="grey" onclick="if(confirm('Start a new test? Current data will be erased.'))post('/reset')">New Test</button>
</div>

<script>
const G2N=0.00980665;let hist=[],busy=false;
const $=id=>document.getElementById(id);
async function post(p){try{const r=await fetch(p,{method:'POST'});if(!r.ok)alert(await r.text());}catch(e){alert('No connection');}poll();}
function ignite(){if(confirm('Area clear? Fire the igniter?'))post('/ignite');}
function cal(){const g=parseFloat($('calg').value);if(!(g>0)){alert('Enter the known weight in grams');return;}post('/cal?g='+g);}
function show(id,on){$(id).classList.toggle('hide',!on);}
function draw(){const c=$('cv'),x=c.getContext('2d'),w=c.width,h=c.height;x.clearRect(0,0,w,h);
 if(hist.length<2)return;let mn=Math.min(0,...hist),mx=Math.max(1,...hist);const p=(mx-mn)*0.1;mn-=p;mx+=p;
 const y=v=>h-(v-mn)/(mx-mn)*h;
 x.strokeStyle='#444';x.beginPath();x.moveTo(0,y(0));x.lineTo(w,y(0));x.stroke();
 x.strokeStyle='#4fc3f7';x.lineWidth=2;x.beginPath();
 hist.forEach((v,i)=>{const px=i/(hist.length-1)*w;i?x.lineTo(px,y(v)):x.moveTo(px,y(v));});x.stroke();
 x.fillStyle='#aaa';x.font='12px sans-serif';x.fillText(mx.toFixed(1)+' N',4,12);x.fillText(mn.toFixed(1)+' N',4,h-4);}
async function poll(){if(busy)return;busy=true;
 try{const d=await (await fetch('/live')).json();
  $('conn').textContent='connected';
  const n=d.g*G2N;
  $('thrust').textContent=n.toFixed(2)+' N';
  $('grams').textContent=d.g.toFixed(0)+' g ('+(d.g/1000).toFixed(3)+' kgf)';
  $('st').textContent=d.s+(d.relay?' | RELAY ON':'');
  $('adc').textContent=d.adc?('ADC '+d.sps+' SPS'):'ADC NO DATA';
  $('el').textContent=(d.s=='RUNNING'||d.s=='BURNOUT')?('t = '+d.el.toFixed(1)+' s'):'';
  $('msg').textContent=d.msg;
  $('cpg').textContent='counts per gram: '+d.cpg.toFixed(3);
  hist.push(n);if(hist.length>300)hist.shift();draw();
  show('pIdle',d.s=='IDLE');show('pRun',d.s=='RUNNING');show('pBurn',d.s=='BURNOUT');show('pDone',d.s=='DONE');
  $('countdown').textContent=d.fireIn>0?('Igniting in '+(d.fireIn/1000).toFixed(1)+' s'):(d.ign?'IGNITION SENT':'');
  $('bBurn').disabled=!d.ign;$('bBurn').style.opacity=d.ign?1:0.4;
  if(d.s=='DONE'){
   $('rPeak').textContent=(d.peak*G2N).toFixed(2)+' N ('+d.peak.toFixed(0)+' g)';
   $('rBurn').textContent=d.burn>=0?d.burn.toFixed(3)+' s':'not marked';
   $('rImp').textContent=d.imp.toFixed(3)+' N·s';
   $('rN').textContent=d.n;}
 }catch(e){$('conn').textContent='disconnected';}
 busy=false;}
setInterval(poll,200);poll();
</script></body></html>
)rawliteral";

// ---------- HTTP handlers ----------
const char* stateName() {
  switch (state) {
    case IDLE: return "IDLE";
    case RUNNING: return "RUNNING";
    case BURNED_OUT: return "BURNOUT";
    default: return "DONE";
  }
}

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleLive() {
  char buf[400];
  float el = (state == RUNNING || state == BURNED_OUT) ? (micros() - testStartUs) / 1e6f : 0;
  long fireIn = firePending ? (long)(int32_t)(fireAtMs - millis()) : 0;
  if (fireIn < 0) fireIn = 0;
  snprintf(buf, sizeof(buf),
    "{\"s\":\"%s\",\"g\":%.1f,\"n\":%d,\"adc\":%d,\"sps\":%d,\"msg\":\"%s\",\"cpg\":%.4f,"
    "\"relay\":%d,\"ign\":%d,\"fireIn\":%ld,\"el\":%.2f,\"peak\":%.1f,\"burn\":%.3f,\"imp\":%.4f}",
    stateName(), (float)liveGrams, (int)sampleCount, adcOk ? 1 : 0, (int)sps, statusMsg,
    (float)countsPerGram, relayOn ? 1 : 0, ignitionFired ? 1 : 0, fireIn, el,
    (float)peakGrams, (float)burnTimeS, (float)impulseNs);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handleIgnite() {
  if (state != IDLE) { server.send(409, "text/plain", "A test is already running"); return; }
  if (!adcOk) { server.send(409, "text/plain", "No data from ADS1232 - check wiring"); return; }
  sampleCount = 0;
  peakGrams = 0;
  burnTimeS = -1;
  impulseNs = 0;
  ignitionFired = false;
  burnoutMarked = false;
  testStartUs = micros();
  fireAtMs = millis() + PRE_IGNITION_MS;
  firePending = true;
  state = RUNNING;
  setMsg("Recording...");
  server.send(200, "text/plain", "OK");
}

void handleBurnout() {
  if (state != RUNNING || !ignitionFired) { server.send(409, "text/plain", "Ignition has not happened yet"); return; }
  burnoutUs = micros();
  burnoutMarked = true;
  state = BURNED_OUT;
  setMsg("Burnout marked");
  server.send(200, "text/plain", "OK");
}

void handleEnd() {
  if (state != RUNNING && state != BURNED_OUT) { server.send(409, "text/plain", "No test running"); return; }
  endTest();
  setMsg("Test ended");
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  if (state != DONE) { server.send(409, "text/plain", "Finish the test first"); return; }
  sampleCount = 0;
  state = IDLE;
  setMsg("Ready - tare before the next test");
  server.send(200, "text/plain", "OK");
}

void handleTare() {
  if (state != IDLE) { server.send(409, "text/plain", "Only when idle"); return; }
  tareRequest = true;
  server.send(200, "text/plain", "OK");
}

void handleCal() {
  if (state != IDLE) { server.send(409, "text/plain", "Only when idle"); return; }
  float g = server.arg("g").toFloat();
  if (g <= 0) { server.send(400, "text/plain", "Enter a weight in grams"); return; }
  calRequest = g;
  server.send(200, "text/plain", "OK");
}

void handleThrustCsv() {
  if (state != DONE || sampleCount == 0) { server.send(409, "text/plain", "No finished test"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"thrust.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("time_s,thrust_N,thrust_g\n");
  String chunk;
  chunk.reserve(2048);
  char line[48];
  for (int i = 0; i < sampleCount; i++) {
    snprintf(line, sizeof(line), "%.4f,%.3f,%.1f\n",
             samples[i].t_us / 1e6, samples[i].grams * G_TO_N, samples[i].grams);
    chunk += line;
    if (chunk.length() > 1800) { server.sendContent(chunk); chunk = ""; }
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");
}

void handleEventsCsv() {
  if (state != DONE) { server.send(409, "text/plain", "No finished test"); return; }
  String s = "event,time_s\nRecording start,0.000\n";
  char line[48];
  if (ignitionFired) {
    snprintf(line, sizeof(line), "Ignition,%.3f\n", (ignitionUs - testStartUs) / 1e6);
    s += line;
  }
  if (burnoutMarked) {
    snprintf(line, sizeof(line), "Burnout,%.3f\n", (burnoutUs - testStartUs) / 1e6);
    s += line;
  }
  snprintf(line, sizeof(line), "End,%.3f\n", (endUs - testStartUs) / 1e6);
  s += line;
  server.sendHeader("Content-Disposition", "attachment; filename=\"events.csv\"");
  server.send(200, "text/csv", s);
}

// ---------- Setup / loop ----------
void setup() {
  // Relay OFF before anything else
  digitalWrite(PIN_RELAY, RELAY_OFF_LEVEL);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF_LEVEL);

  Serial.begin(115200);
  pinMode(PIN_SCLK, OUTPUT);
  digitalWrite(PIN_SCLK, LOW);
  pinMode(PIN_DOUT, INPUT);

  samples = (Sample*)malloc(sizeof(Sample) * MAX_SAMPLES);
  if (!samples) { Serial.println("Not enough RAM"); while (true) delay(1000); }

  prefs.begin("stand", false);
  countsPerGram = prefs.getFloat("cpg", DEFAULT_COUNTS_PER_GRAM);

  delay(500);
  long dummy;
  if (readRawBlocking(dummy, 2)) Serial.println("ADS1232 found, self-calibrating");
  else Serial.println("No data from ADS1232 - check wiring");
  delay(1500);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Connect to Wi-Fi '");
  Serial.print(AP_SSID);
  Serial.print("' and open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/live", HTTP_GET, handleLive);
  server.on("/ignite", HTTP_POST, handleIgnite);
  server.on("/burnout", HTTP_POST, handleBurnout);
  server.on("/end", HTTP_POST, handleEnd);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/tare", HTTP_POST, handleTare);
  server.on("/cal", HTTP_POST, handleCal);
  server.on("/thrust.csv", HTTP_GET, handleThrustCsv);
  server.on("/events.csv", HTTP_GET, handleEventsCsv);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  tareRequest = true;   // auto-tare at startup
  setMsg("Ready");
  xTaskCreatePinnedToCore(adcTask, "adc", 4096, nullptr, 2, nullptr, 1);
}

void loop() {
  uint32_t now = millis();

  // Fire relay after the pre-ignition baseline
  if (firePending && (int32_t)(now - fireAtMs) >= 0) {
    firePending = false;
    ignitionUs = micros();
    ignitionFired = true;
    relaySet(true);
    relayOffAtMs = now + IGNITER_ON_MS;
    setMsg("IGNITION");
  }
  // Auto relay off
  if (relayOn && (int32_t)(now - relayOffAtMs) >= 0) relaySet(false);

  // Save calibration to flash
  if (calSavePending) {
    calSavePending = false;
    prefs.putFloat("cpg", countsPerGram);
  }

  server.handleClient();
}