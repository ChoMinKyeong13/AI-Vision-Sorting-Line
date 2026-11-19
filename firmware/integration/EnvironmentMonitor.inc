// Included once inside MainAux. No second Serial parser, delay(), dynamic
// allocation or long global interrupt lock. DHT response uses external IRQ D18
// (Arduino interrupt index 5, ATmega2560 hardware INT3).
namespace Environment {
enum EnvState : uint8_t { NORMAL, WARNING, CRITICAL, SENSOR_ERROR };
enum ReceivePhase : uint8_t { RX_OFF, RESPONSE_LOW, RESPONSE_HIGH, RESPONSE_END,
                              BIT_RISE, BIT_FALL, RX_DONE, RX_BAD };
enum SamplePhase : uint8_t { SAMPLE_IDLE, START_LOW, RECEIVING };
volatile uint8_t receivePhase = RX_OFF, receivedBits = 0, received[5] = {};
volatile uint32_t edgeAtUs = 0;
SamplePhase samplePhase = SAMPLE_IDLE;
EnvState state = SENSOR_ERROR;
int16_t temperature = 0, humidity = 0;
int16_t warningTemperature = 280, warningHumidity = 650;
int16_t criticalTemperature = 350, criticalHumidity = 800;
bool valid = false, muted = false;
uint32_t lastSampleAt = 0, lastGoodAt = 0, lastReportAt = 0;
uint32_t startAtMs = 0, receiveAtUs = 0, sequence = 0;
bool toneOn = false;
uint16_t toneFrequency = 0;
#if !defined(__AVR__)
// Host-only hardware boundary; existing MCU fixtures need no new Arduino stubs.
uint16_t hostSubMillisecondUs = 0, hostToneHz = 0;
bool hostInterruptAttached = false;
#endif

uint32_t nowUs() {
#if defined(__AVR__)
  return micros();
#else
  return static_cast<uint32_t>(millis()) * 1000UL + hostSubMillisecondUs;
#endif
}
void detachInput() {
#if defined(__AVR__)
  detachInterrupt(digitalPinToInterrupt(MainAuxSettings::ENV_DHT_PIN));
#else
  hostInterruptAttached = false;
#endif
}
void stopTone() {
#if defined(__AVR__)
  noTone(MainAuxSettings::ENV_BUZZER_PIN);
#else
  hostToneHz = 0;
#endif
}
void startTone(uint16_t frequency) {
#if defined(__AVR__)
  tone(MainAuxSettings::ENV_BUZZER_PIN, frequency);
#else
  hostToneHz = frequency;
#endif
}

// ISR owns these five bytes until RX_DONE/RX_BAD. The main loop detaches the
// interrupt before copying them. No 32-bit ISR fields are read concurrently.
void captureEdge(bool high, uint32_t atUs) {
  const uint8_t phase = receivePhase;
  if (phase == RX_OFF || phase == RX_DONE || phase == RX_BAD) return;
  const uint32_t width = atUs - edgeAtUs;
  if (phase == RESPONSE_LOW) {
    if (!high) { edgeAtUs = atUs; receivePhase = RESPONSE_HIGH; }
    return;
  }
  if (phase == RESPONSE_HIGH) {
    if (!high || width < 50 || width > 120) { receivePhase = RX_BAD; return; }
    edgeAtUs = atUs; receivePhase = RESPONSE_END; return;
  }
  if (phase == RESPONSE_END) {
    if (high || width < 50 || width > 120) { receivePhase = RX_BAD; return; }
    edgeAtUs = atUs; receivePhase = BIT_RISE; return;
  }
  if (phase == BIT_RISE) {
    if (!high || width < 30 || width > 90) { receivePhase = RX_BAD; return; }
    edgeAtUs = atUs; receivePhase = BIT_FALL; return;
  }
  if (high || width < 10 || width > 100 || receivedBits >= 40) { receivePhase = RX_BAD; return; }
  const uint8_t slot = receivedBits >> 3;
  received[slot] = static_cast<uint8_t>((received[slot] << 1) | (width >= 50 ? 1 : 0));
  ++receivedBits; edgeAtUs = atUs;
  receivePhase = receivedBits == 40 ? RX_DONE : BIT_RISE;
}
#if defined(__AVR__)
void onDataEdge() { captureEdge(digitalRead(MainAuxSettings::ENV_DHT_PIN) == HIGH, micros()); }
#endif

const char *stateName() {
  return state == NORMAL ? "NORMAL" : state == WARNING ? "WARNING" :
         state == CRITICAL ? "CRITICAL" : "SENSOR_ERROR";
}
EnvState evaluate(int16_t t, int16_t h) {
  if (t >= criticalTemperature || h >= criticalHumidity ||
      (state == CRITICAL && (t > criticalTemperature - 20 || h > criticalHumidity - 40))) return CRITICAL;
  if (t >= warningTemperature || h >= warningHumidity ||
      ((state == WARNING || state == CRITICAL) &&
       (t > warningTemperature - 10 || h > warningHumidity - 30))) return WARNING;
  return NORMAL;
}
void acceptFrame(uint32_t completedAtMs) {
  uint8_t data[5];
  for (uint8_t i = 0; i < 5; ++i) data[i] = received[i];
  if (receivedBits != 40 || static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]) != data[4] ||
      data[1] > 9 || (data[3] & 0x0f) > 9 || (data[3] & 0x70)) return;
  const int16_t h = static_cast<int16_t>(data[0]) * 10 + data[1];
  // Matches the DHT11 byte interpretation of the submitted DHT.h example.
  const int16_t t = ((data[3] & 0x80) ? -10 - static_cast<int16_t>(data[2]) * 10 :
                     static_cast<int16_t>(data[2]) * 10) + (data[3] & 0x0f);
  if (h < 0 || h > 1000 || t <= -200 || t >= 800) return;
  // Use the same loop snapshot as expiry evaluation. Re-reading millis() here
  // could put lastGoodAt 1 ms ahead of that snapshot and falsely wrap its age.
  temperature = t; humidity = h; valid = true; lastGoodAt = completedAtMs; state = evaluate(t, h);
}
void sample(uint32_t now) {
  if (samplePhase == SAMPLE_IDLE) {
    if (now - lastSampleAt < MainAuxSettings::ENV_SAMPLE_MS) return;
    lastSampleAt = now; startAtMs = now;
    detachInput(); receivePhase = RX_OFF;
    digitalWrite(MainAuxSettings::ENV_DHT_PIN, LOW);
    pinMode(MainAuxSettings::ENV_DHT_PIN, OUTPUT);
    samplePhase = START_LOW; return;
  }
  if (samplePhase == START_LOW) {
    if (now - startAtMs < 20) return; // DHT11 >=18 ms LOW; loop/STOP stays live.
    for (uint8_t i = 0; i < 5; ++i) received[i] = 0;
    receivedBits = 0; receiveAtUs = nowUs(); edgeAtUs = receiveAtUs;
    receivePhase = RESPONSE_LOW; samplePhase = RECEIVING;
#if defined(__AVR__)
    // Arduino attachInterrupt() enables INT3 without clearing its pending flag.
    // Our own START falling edge may already be pending while masked. Release
    // DATA and discard only that host edge before enabling global interrupts.
    // This short register/GPIO sequence never covers the 20 ms START interval.
    const uint8_t savedSreg = SREG;
    cli();
    attachInterrupt(digitalPinToInterrupt(MainAuxSettings::ENV_DHT_PIN), onDataEdge, CHANGE);
    pinMode(MainAuxSettings::ENV_DHT_PIN, INPUT_PULLUP);
    EIFR = _BV(INTF3); // D18 is hardware INT3; preserve all other pending IRQs.
    SREG = savedSreg;
#else
    hostInterruptAttached = true;
    pinMode(MainAuxSettings::ENV_DHT_PIN, INPUT);
    digitalWrite(MainAuxSettings::ENV_DHT_PIN, HIGH);
#endif
    return;
  }
  const uint8_t phase = receivePhase;
  if (phase == RX_DONE || phase == RX_BAD || static_cast<uint32_t>(nowUs() - receiveAtUs) >= 8000UL) {
    detachInput();
    if (phase == RX_DONE) acceptFrame(now);
    receivePhase = RX_OFF; samplePhase = SAMPLE_IDLE;
  }
}
void formatTenths(int16_t value, char *out, size_t capacity) {
  const uint16_t magnitude = value < 0 ? static_cast<uint16_t>(-value) : static_cast<uint16_t>(value);
  snprintf(out, capacity, "%s%u.%u", value < 0 ? "-" : "", magnitude / 10, magnitude % 10);
}
void report(uint32_t now) {
  // An explicit query must never revive an aged sample between loop services.
  if (valid && now - lastGoodAt >= MainAuxSettings::ENV_STALE_MS) { valid = false; state = SENSOR_ERROR; }
  char t[9], h[9];
  if (valid) { formatTenths(temperature, t, sizeof(t)); formatTenths(humidity, h, sizeof(h)); }
  else { strcpy(t, "nan"); strcpy(h, "nan"); }
  lastReportAt = now;
  LINK_LOG("ENV,1,%lu,%lu,%s,%s,%s,%u,%u", ++sequence, now, t, h, stateName(), muted ? 1 : 0, valid ? 1 : 0);
}
void buzzer(uint32_t now) {
  bool on = false; uint16_t frequency = 0;
  if (MainAuxSettings::ENV_BUZZER_ENABLED && !muted) {
    const uint16_t phase = now % 2000UL;
    if (state == WARNING) { on = phase < 180; frequency = 2000; }
    else if (state == CRITICAL) { on = phase < 220 || (phase >= 360 && phase < 580); frequency = 2500; }
    else if (state == SENSOR_ERROR) { on = phase < 130; frequency = 1200; }
  }
  if (on && (!toneOn || frequency != toneFrequency)) startTone(frequency);
  else if (!on && toneOn) stopTone();
  toneOn = on; toneFrequency = on ? frequency : 0;
}
void begin() {
  detachInput(); receivePhase = RX_OFF; receivedBits = 0;
  samplePhase = SAMPLE_IDLE; state = SENSOR_ERROR; valid = false; muted = false;
  temperature = humidity = 0;
  warningTemperature = 280; warningHumidity = 650; criticalTemperature = 350; criticalHumidity = 800;
  lastSampleAt = lastReportAt = startAtMs = millis(); lastGoodAt = receiveAtUs = sequence = 0;
  toneOn = false; toneFrequency = 0;
#if !defined(__AVR__)
  hostSubMillisecondUs = 0;
#endif
  pinMode(MainAuxSettings::ENV_DHT_PIN, INPUT);
  digitalWrite(MainAuxSettings::ENV_DHT_PIN, HIGH); // input pull-up, never drive the sensor HIGH.
  digitalWrite(MainAuxSettings::ENV_BUZZER_PIN, LOW); pinMode(MainAuxSettings::ENV_BUZZER_PIN, OUTPUT); stopTone();
  LINK_LOG("ENV_READY,DHT11,MEGA_INTEGRATED,115200");
}
bool parseTenths(const char *&cursor, int16_t &value, bool last) {
  // Decimal thresholds at 0.1 precision; additional trailing zeroes are harmless.
  uint16_t integer = 0; uint8_t digits = 0, fractional = 0;
  while (*cursor >= '0' && *cursor <= '9') {
    if (++digits > 3) return false;
    integer = integer * 10 + static_cast<uint8_t>(*cursor++ - '0');
  }
  if (!digits) return false;
  if (*cursor == '.') {
    ++cursor;
    if (*cursor < '0' || *cursor > '9') return false;
    fractional = static_cast<uint8_t>(*cursor++ - '0');
    while (*cursor >= '0' && *cursor <= '9') if (*cursor++ != '0') return false;
  }
  if (integer > 100 || (last ? *cursor != '\0' : *cursor != ',')) return false;
  value = static_cast<int16_t>(integer * 10 + fractional);
  if (!last) ++cursor;
  return true;
}
bool handleCommand(const char *line) {
  if (!strcmp(line, "MUTE") || !strcmp(line, "UNMUTE")) {
    muted = line[0] == 'M'; buzzer(millis()); LINK_LOG("ENV_ACK,%s", muted ? "MUTE" : "UNMUTE"); return true;
  }
  if (!strcmp(line, "ENVSTATUS")) { report(millis()); return true; }
  if (strncmp(line, "ENV_SET", 7)) return false;
  if (line[7] != ',') { LINK_LOG("ENV_ERR,BAD_THRESHOLDS"); return true; }
  int16_t values[4]; const char *cursor = line + 8;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!parseTenths(cursor, values[i], i == 3)) { LINK_LOG("ENV_ERR,BAD_THRESHOLDS"); return true; }
  }
  if (values[0] >= values[2] || values[2] > 600 || values[1] >= values[3] || values[3] > 1000) {
    LINK_LOG("ENV_ERR,BAD_THRESHOLDS"); return true;
  }
  // Alarm settings are independent from motion and STOP/NC recovery. The shared
  // dispatcher still samples NC and handles STOP before reaching this command.
  warningTemperature = values[0]; warningHumidity = values[1]; criticalTemperature = values[2]; criticalHumidity = values[3];
  if (valid) state = evaluate(temperature, humidity);
  LINK_LOG("ENV_ACK,SET,%u.%u,%u.%u,%u.%u,%u.%u", values[0] / 10, values[0] % 10,
      values[1] / 10, values[1] % 10, values[2] / 10, values[2] % 10, values[3] / 10, values[3] % 10);
  const uint32_t now = millis();
  report(now); // Expire an aged reading before updating the alarm output.
  buzzer(now); return true;
}
void update() {
  const uint32_t now = millis();
  sample(now);
  if (valid && now - lastGoodAt >= MainAuxSettings::ENV_STALE_MS) { valid = false; state = SENSOR_ERROR; }
  buzzer(now);
  if (now - lastReportAt >= MainAuxSettings::ENV_REPORT_MS) report(now);
}
} // namespace Environment
