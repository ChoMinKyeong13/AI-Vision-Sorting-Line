#include <Arduino.h>
#include "AuxConveyor.h"
#include <Servo.h>
#include <avr/pgmspace.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#if !defined(__AVR_ATmega2560__) && !defined(__AVR_ATmega328P__)
#error "Select Arduino Mega2560 or Uno."
#endif

// Currently wired to D11 on Uno; confirm pin availability on final Mega.
#ifndef AUX_GUIDE_PIN
#define AUX_GUIDE_PIN 11
#endif
const uint8_t AUX_IA = 5, AUX_IB = 6; // Existing AUX mapping; check actual wiring.
const bool AUX_INVERT_OUTPUT = false; // Electrical +/- are not verified bin directions.
const int GUIDE_MIN_DEG = 0, GUIDE_MAX_DEG = 180;
const uint16_t DEFAULT_SPEED_DPS = 60, MAX_SPEED_DPS = 360;
const int PWM_LIMIT = 192;
const uint32_t MAX_RUN_MS = 10000UL, RESTART_GAP_MS = 1000UL;
const uint32_t SERVO_TICK_MS = 20UL, MAX_STEP_MS = 100UL;
const uint32_t LINE_TIMEOUT_MS = 1000UL;
const uint8_t LINE_SIZE = 64, RX_BUDGET = 16, TX_BUDGET = 16;
const uint16_t LOG_SIZE = 256; // Uno SRAM limit; logs may be dropped
// Automatic route = PRESET guide -> wait for command ramp/settling -> PWM -> stop.
// The software cannot confirm physical motion or successful collection.
const uint32_t AUTO_GUIDE_SETTLE_MS = 400UL;
const uint32_t AUTO_MAX_GUIDE_WAIT_MS = 15000UL;
const uint32_t AUTO_MAX_OUTPUT_WAIT_MS = 2500UL;
const uint32_t AUTO_MIN_RUN_MS = 300UL, AUTO_MAX_RUN_MS = 9500UL;

// Software test limits above are NOT physical stop/speed/position guarantees.

struct BeltState {
  int output, pending;
  bool hasStopped;
  uint32_t startedAt, stoppedAt;
};
struct GuideState {
  Servo servo;
  bool pinValid, known, moving;
  int minDeg, maxDeg;
  int32_t target, output; // Milli command-degrees, not measured shaft position.
  uint32_t lastTick;
};
enum Slot : uint8_t { CENTER_SLOT, LEFT_SLOT, RIGHT_SLOT, SLOT_COUNT };
enum AutoPhase : uint8_t {
  AUTO_IDLE, AUTO_GUIDE, AUTO_SETTLE, AUTO_WAIT_OUTPUT, AUTO_RUNNING,
  AUTO_AWAIT_EMPTY
};
struct AutoState {
  AutoPhase phase;
  bool armed, needsEmptyConfirmation;
  int8_t defectDirection, holdDirection;  // 0=UNSET; +1 / -1 physical direction verified on bench.
  int pwm;
  uint32_t runMs;
  int32_t target;
  int runPwm;
  const char *route;
  uint32_t phaseAt, runStartedAt;
};
BeltState belt;
GuideState guide;
AutoState auxAuto;
// Separate call-based interface for integration with the MAIN conveyor on THIS Mega.
// Original manual and autoroute commands are retained for bench testing.
enum AuxApiPhase : uint8_t {
  API_IDLE, API_PREPARING, API_SETTLING, API_PREPARED,
  API_WAIT_OUTPUT, API_RUNNING, API_DONE, API_CANCELLED
};
struct AuxApiState {
  AuxApiPhase phase;
  AuxRoute route;
  int32_t target;
  int runPwm;
  uint32_t phaseAt, startedAt;
};
AuxApiState auxApi = {API_IDLE, AUX_LEFT, 0, 0, 0, 0};
int32_t positions[SLOT_COUNT] = {-1, -1, -1}; // RAM only, explicit registration.
uint16_t speedDps = DEFAULT_SPEED_DPS;
bool stopLatched = false;
char inputLine[LINE_SIZE];
uint8_t inputLength = 0;
bool discardLine = false;
uint32_t lastInputAt = 0;
char logBuffer[LOG_SIZE];
uint16_t logHead = 0, logTail = 0;
uint32_t droppedLogs = 0;

uint32_t elapsedMs(uint32_t now, uint32_t since) { return now - since; }

// Same bounded, nonblocking logging pattern as sensor_stop_timing_test.
// A whole line is dropped if full; device timers never wait for PC output.
void queueLog_P(PGM_P format, ...) {
  char line[192];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf_P(line, sizeof(line), format, args);
  va_end(args);
  const uint16_t used = (logHead + LOG_SIZE - logTail) % LOG_SIZE;
  if (length < 0 || length >= (int)sizeof(line) ||
      (uint16_t)(length + 1) > LOG_SIZE - 1 - used) {
    ++droppedLogs; return;
  }
  for (int i = 0; i < length; ++i) {
    logBuffer[logHead] = line[i]; logHead = (logHead + 1) % LOG_SIZE;
  }
  logBuffer[logHead] = '\n'; logHead = (logHead + 1) % LOG_SIZE;
}
#define LOG(format, ...) queueLog_P(PSTR(format), ##__VA_ARGS__)

void pumpLogs() {
  for (uint8_t n = 0; n < TX_BUDGET && logTail != logHead; ++n) {
    if (Serial.availableForWrite() <= 0) break;
    Serial.write((uint8_t)logBuffer[logTail]);
    logTail = (logTail + 1) % LOG_SIZE;
  }
}
bool beltIdle() { return belt.output == 0 && belt.pending == 0; }
bool guideConfigValid() {
  return guide.pinValid && guide.minDeg >= 0 &&
         guide.minDeg < guide.maxDeg && guide.maxDeg <= 180;
}
bool guidePinAllowed(int pin) {
  if (pin < 2 || pin >= NUM_DIGITAL_PINS) return false;
  // Reserve project motor/main-servo/TOP/SIDE signals and USB Serial.
  const uint8_t reserved[] = {2, 3, 5, 6, 12, 13, 22, 24};
  for (uint8_t i = 0; i < sizeof(reserved); ++i)
    if (pin == reserved[i]) return false;
  return true;
}
void writeBelt() {
  analogWrite(AUX_IA, 0); analogWrite(AUX_IB, 0);
  const int value = AUX_INVERT_OUTPUT ? -belt.output : belt.output;
  if (value > 0) analogWrite(AUX_IA, value);
  else if (value < 0) analogWrite(AUX_IB, -value);
}
void stopBelt() {
  const bool wasOn = belt.output != 0;
  belt.output = belt.pending = 0;
  writeBelt();
  if (wasOn) { belt.hasStopped = true; belt.stoppedAt = (uint32_t)millis(); }
}
void startBelt(int value) {
  belt.output = value; belt.pending = 0;
  writeBelt();
  belt.startedAt = (uint32_t)millis(); // Actual PWM application, not measured motion.
  LOG("AUX OUTPUT_ON PWM=%d AT_MS=%lu", value, (unsigned long)belt.startedAt);
}
void haltGuide(bool release) {
  guide.moving = false;
  if (guide.known) guide.target = guide.output;
  if (release) {
    if (guide.servo.attached()) guide.servo.detach();
    if (guide.pinValid) digitalWrite((uint8_t)AUX_GUIDE_PIN, LOW);
    guide.known = false;
  }
}
void stopAll(bool release) {
  if (auxApi.phase != API_IDLE && auxApi.phase != API_DONE) {
    auxApi.phase = API_CANCELLED;
    auxAuto.needsEmptyConfirmation = true;
  }
  if (auxAuto.phase != AUTO_IDLE) auxAuto.needsEmptyConfirmation = true;
  auxAuto.phase = AUTO_IDLE;
  auxAuto.armed = false;
  stopBelt(); stopLatched = true; haltGuide(release);
  LOG("STOP_LATCH=1 OUTPUT_OFF AT_MS=%lu PHYSICAL_STOP=UNMEASURED",
      (unsigned long)(uint32_t)millis());
  if (release) LOG("DETACHED: guide signal OFF; holding torque may disappear");
  LOG("Send resume, then a NEW command. Old movement will not restart.");
}
void inputError(const char *reason) {
  if (auxApi.phase != API_IDLE) auxCancel();
  else stopAll(false);
  LOG("INPUT_ERROR=%s; software STOP only, not hardware emergency stop", reason);
}
void requestBelt(int value) {
  if (value == 0) { stopBelt(); LOG("AUX OUTPUT_OFF: requested; pending canceled"); return; }
  if (stopLatched) { LOG("REJECT: stop latched; resume then NEW b command"); return; }
  if (guide.moving) { LOG("REJECT: guide command ramp active; belt unchanged"); return; }
  if (belt.output && ((belt.output > 0) == (value > 0))) {
    belt.output = value; writeBelt();
    LOG("AUX PWM_CHANGED=%d; run deadline NOT extended", value); return;
  }
  if (belt.output) stopBelt();
  if (belt.hasStopped && elapsedMs((uint32_t)millis(), belt.stoppedAt) < RESTART_GAP_MS) {
    belt.pending = value;
    LOG("AUX QUEUED PWM=%d: 1000ms output-OFF gap; b0/stop cancels", value);
  } else startBelt(value);
}
void serviceBelt() {
  if (stopLatched) return;
  const uint32_t now = (uint32_t)millis();
  if (belt.output && elapsedMs(now, belt.startedAt) >= MAX_RUN_MS) {
    stopBelt(); LOG("AUX OUTPUT_OFF: 10s run limit; NEW request required");
  } else if (!belt.output && belt.pending &&
             elapsedMs(now, belt.stoppedAt) >= RESTART_GAP_MS) {
    const int value = belt.pending;
    if (guide.moving) { stopBelt(); LOG("AUX QUEUE_CANCELED: guide moving"); }
    else startBelt(value);
  }
}
void writeGuide() {
  const int pulse = MIN_PULSE_WIDTH +
      (int)(guide.output * (MAX_PULSE_WIDTH - MIN_PULSE_WIDTH) / 180000L);
  // Explicit target cached BEFORE attach, matching the preserved manual bench.
  guide.servo.writeMicroseconds(pulse);
  if (!guide.servo.attached())
    guide.servo.attach(AUX_GUIDE_PIN, MIN_PULSE_WIDTH, MAX_PULSE_WIDTH);
}
bool requestGuide(int32_t target) {
  if (stopLatched) { LOG("REJECT: stop latched; resume then NEW guide command"); return false; }
  if (!guideConfigValid()) { LOG("REJECT: guide pin UNSET/reserved or invalid range"); return false; }
  if (!beltIdle()) { LOG("REJECT: b0 first; guide must not change while AUX running/queued"); return false; }
  if (target < (int32_t)guide.minDeg * 1000L ||
      target > (int32_t)guide.maxDeg * 1000L) {
    LOG("REJECT: guide target outside range; previous target unchanged"); return false;
  }
  const bool first = !guide.known || !guide.servo.attached();
  guide.target = target; guide.known = true; guide.lastTick = (uint32_t)millis();
  if (first || speedDps == 0) {
    guide.output = target; guide.moving = false; writeGuide();
    if (first) LOG("FIRST_TARGET: physical start unknown; first move cannot be speed-limited");
  } else guide.moving = guide.output != target;
  return true;
}
void serviceGuide() {
  if (stopLatched || !guide.moving) return;
  const uint32_t now = (uint32_t)millis();
  uint32_t elapsed = elapsedMs(now, guide.lastTick);
  if (elapsed < SERVO_TICK_MS) return;
  guide.lastTick = now;
  if (elapsed > MAX_STEP_MS) elapsed = MAX_STEP_MS;
  const int32_t step = (int32_t)speedDps * (int32_t)elapsed;
  const int32_t distance = guide.target - guide.output;
  if (distance >= -step && distance <= step) {
    guide.output = guide.target; guide.moving = false;
  } else guide.output += distance > 0 ? step : -step;
  writeGuide();
  if (!guide.moving) LOG("GUIDE COMMAND_REACHED; PHYSICAL_POSITION=UNMEASURED");
}
// These route names are PHYSICAL destinations, not quality labels.
// The full MCU owner must map labels/pin-missing after agreeing on actual bins.
const char *autoPhaseName() {
  switch (auxAuto.phase) {
    case AUTO_IDLE: return "IDLE";
    case AUTO_GUIDE: return "GUIDE_COMMAND";
    case AUTO_SETTLE: return "SETTLE_WAIT";
    case AUTO_WAIT_OUTPUT: return "WAIT_OUTPUT";
    case AUTO_RUNNING: return "PWM_ON";
    case AUTO_AWAIT_EMPTY: return "AWAIT_OPERATOR_EMPTY";
    default: return "INVALID";
  }
}
bool autoLocked() {
  return auxAuto.phase != AUTO_IDLE || auxAuto.needsEmptyConfirmation;
}
void autoFault(const char *reason) {
  LOG("AUTO_FAULT=%s: OUTPUT_OFF, physical exit/empty UNKNOWN", reason);
  stopAll(false); // Latched; requires manual inspection, verifyempty and resume.
}
void requestAutoRoute(const char *route) {
  if (auxApi.phase != API_IDLE) {
    LOG("AUTO_REJECT: function API owns auxiliary; finish and confirm empty first"); return;
  }
  if (!auxAuto.armed || stopLatched || autoLocked()) {
    LOG("AUTO_REJECT: autoarm required, STOP/unfinished prior cycle/empty unknown"); return;
  }
  if (!beltIdle() || guide.moving || !guideConfigValid() ||
      !guide.known || !guide.servo.attached()) {
    LOG("AUTO_REJECT: AUX OFF + known/attached stationary guide required"); return;
  }
  if (!auxAuto.runMs || !auxAuto.pwm) {
    LOG("AUTO_REJECT: set autorunms and autopwm after bench calibration"); return;
  }
  uint8_t slot = SLOT_COUNT;
  int direction = 0;
  const char *stableRoute = "none"; // Do not retain pointer into Serial line buffer.
  if (!strcmp(route, "left")) { slot = LEFT_SLOT; direction = auxAuto.defectDirection; stableRoute = "left"; }
  else if (!strcmp(route, "right")) { slot = RIGHT_SLOT; direction = auxAuto.defectDirection; stableRoute = "right"; }
  else if (!strcmp(route, "end")) { slot = CENTER_SLOT; direction = auxAuto.defectDirection; stableRoute = "end"; }
  else if (!strcmp(route, "hold")) { slot = CENTER_SLOT; direction = auxAuto.holdDirection; stableRoute = "hold"; }
  else { LOG("AUTO_REJECT: route is left/right/end/hold only"); return; }
  if (!direction || positions[slot] < 0 ||
      positions[slot] < (int32_t)guide.minDeg * 1000L ||
      positions[slot] > (int32_t)guide.maxDeg * 1000L) {
    LOG("AUTO_REJECT: route dir UNSET or CENTER/LEFT/RIGHT angle UNSET/out of range"); return;
  }
  auxAuto.route = stableRoute;
  auxAuto.target = positions[slot];
  auxAuto.runPwm = direction * auxAuto.pwm;
  if (!requestGuide(auxAuto.target)) return;
  auxAuto.armed = false;
  auxAuto.needsEmptyConfirmation = true; // Human must verify after motion/interruption.
  auxAuto.phase = AUTO_GUIDE;
  auxAuto.phaseAt = (uint32_t)millis();
  LOG("AUTO_ACCEPT ROUTE=%s TARGET_CMD_MDEG=%ld PWM=%d DURATION_MS=%lu",
      route, (long)auxAuto.target, auxAuto.runPwm, (unsigned long)auxAuto.runMs);
  LOG("AUTO_PREPARE: keep upstream feeding inhibited; guide position not measured");
}
void serviceAuto() {
  const uint32_t now = (uint32_t)millis();
  switch (auxAuto.phase) {
    case AUTO_IDLE:
    case AUTO_AWAIT_EMPTY: return;
    case AUTO_GUIDE:
      if (elapsedMs(now, auxAuto.phaseAt) >= AUTO_MAX_GUIDE_WAIT_MS) {
        autoFault("GUIDE_COMMAND_TIMEOUT"); return;
      }
      if (!guide.moving && guide.known && guide.output == auxAuto.target) {
        auxAuto.phase = AUTO_SETTLE; auxAuto.phaseAt = now;
        LOG("AUTO_GUIDE_COMMAND_REACHED: wait %lu ms; PHYSICAL_POSITION_UNMEASURED",
            (unsigned long)AUTO_GUIDE_SETTLE_MS);
      }
      return;
    case AUTO_SETTLE:
      if (guide.moving || !beltIdle()) { autoFault("UNEXPECTED_MOTION_DURING_SETTLE"); return; }
      if (elapsedMs(now, auxAuto.phaseAt) >= AUTO_GUIDE_SETTLE_MS) {
        requestBelt(auxAuto.runPwm); // Respects original 1s OFF reversal gap.
        if (stopLatched || (belt.output != auxAuto.runPwm && belt.pending != auxAuto.runPwm)) {
          autoFault("BELT_START_REJECTED"); return;
        }
        auxAuto.phase = AUTO_WAIT_OUTPUT; auxAuto.phaseAt = now;
        LOG("AUTO_OUTPUT_REQUESTED: upstream transfer needs separate coordination");
      }
      return;
    case AUTO_WAIT_OUTPUT:
      if (belt.output == auxAuto.runPwm && belt.pending == 0) {
        auxAuto.phase = AUTO_RUNNING;
        auxAuto.runStartedAt = belt.startedAt; // PWM applied, NOT physical motion measured.
        LOG("AUTO_PWM_ON AT_MS=%lu", (unsigned long)auxAuto.runStartedAt);
      } else if (elapsedMs(now, auxAuto.phaseAt) >= AUTO_MAX_OUTPUT_WAIT_MS) {
        autoFault("OUTPUT_START_TIMEOUT");
      }
      return;
    case AUTO_RUNNING:
      if (elapsedMs(now, auxAuto.runStartedAt) >= auxAuto.runMs) {
        stopBelt();
        auxAuto.phase = AUTO_AWAIT_EMPTY;
        LOG("AUTO_SEQUENCE_END ROUTE=%s PWM_OFF; EXIT_UNVERIFIED, operator check then verifyempty",
            auxAuto.route);
      } else if (belt.output != auxAuto.runPwm) {
        autoFault("OUTPUT_INTERRUPTED");
      }
      return;
  }
}
// ===== NEW: call-based integration API for ONE Mega =====
// Calling prepare only positions the guide; it does NOT start the auxiliary belt.
// The test settings are retained in RAM after setup until reboot / reconfiguration.
void auxLoadTestedConfiguration() {
  // No motor or servo output is requested here. Confirm actual mechanics before use.
  if (auxApi.phase != API_IDLE || auxAuto.phase != AUTO_IDLE || !beltIdle()) return;
  positions[CENTER_SLOT] = 5000L;      // 5 deg: END / HOLD
  positions[LEFT_SLOT]   = 60000L;     // 60 deg
  positions[RIGHT_SLOT]  = 157000L;    // 157 deg: operator-adjusted RIGHT route
  auxAuto.defectDirection = -1;
  auxAuto.holdDirection = 1;
  auxAuto.pwm = 192;
  auxAuto.runMs = 9500UL;
  LOG("AUX_CONFIG_LOADED C=5 L=60 R=157 DEFECT=-192 HOLD=+192 RUN_MS=9500; NO MOVEMENT");
}

bool auxPrepare(AuxRoute route) {
  if (auxApi.phase != API_IDLE || stopLatched || autoLocked() || auxAuto.armed ||
      !beltIdle() || guide.moving || !guideConfigValid() ||
      auxAuto.pwm != 192 || auxAuto.runMs != 9500UL) {
    LOG("AUX_API_PREPARE_REJECTED: busy/stop/empty/config"); return false;
  }
  uint8_t slot;
  int direction;
  switch (route) {
    case AUX_LEFT:  slot = LEFT_SLOT;   direction = auxAuto.defectDirection; break;
    case AUX_RIGHT: slot = RIGHT_SLOT;  direction = auxAuto.defectDirection; break;
    case AUX_END:   slot = CENTER_SLOT; direction = auxAuto.defectDirection; break;
    case AUX_HOLD:  slot = CENTER_SLOT; direction = auxAuto.holdDirection;   break;
    default: return false;
  }
  if ((direction != 1 && direction != -1) ||
      positions[slot] < (int32_t)guide.minDeg * 1000L ||
      positions[slot] > (int32_t)guide.maxDeg * 1000L) return false;
  const int32_t target = positions[slot];
  if (!requestGuide(target)) return false;
  auxApi.route = route;
  auxApi.target = target;
  auxApi.runPwm = direction * auxAuto.pwm;
  auxApi.phase = API_PREPARING;
  auxApi.phaseAt = (uint32_t)millis();
  // Blocking a new cycle until the operator actually checks that the chute is empty.
  auxAuto.needsEmptyConfirmation = true;
  LOG("AUX_API_PREPARE_ACCEPT ROUTE=%u TARGET_MDEG=%ld; BELT_OFF",
      (unsigned int)route, (long)target);
  return true;
}

bool auxIsPrepared() { return auxApi.phase == API_PREPARED; }

bool auxStart() {
  if (stopLatched || auxApi.phase != API_PREPARED || guide.moving ||
      !guide.known || guide.output != auxApi.target || !beltIdle()) return false;
  requestBelt(auxApi.runPwm);  // Includes existing 1 second reverse-direction OFF gap.
  if (stopLatched ||
      (belt.output != auxApi.runPwm && belt.pending != auxApi.runPwm)) {
    stopAll(false); return false;
  }
  auxApi.phase = API_WAIT_OUTPUT;
  auxApi.phaseAt = (uint32_t)millis();
  LOG("AUX_API_START_ACCEPT PWM=%d; motor output may be queued", auxApi.runPwm);
  return true;
}

void auxCancel() {
  // Cancel is distinct from a normal successful finish and always latches stop.
  if (auxApi.phase != API_IDLE) auxAuto.needsEmptyConfirmation = true;
  stopAll(false);
  auxApi.phase = API_CANCELLED;
  LOG("AUX_API_CANCELLED: inspect path, confirm empty, then resume");
}

bool auxIsDone() {
  // Means commanded motor sequence finished, NOT that product physically arrived.
  return auxApi.phase == API_DONE;
}

bool auxIsReady() {
  return auxApi.phase == API_IDLE && auxAuto.phase == AUTO_IDLE &&
         !auxAuto.armed && !auxAuto.needsEmptyConfirmation && !stopLatched &&
         beltIdle() && !guide.moving;
}

bool auxConfirmEmpty() {
  // Call ONLY after an operator visually confirms belt + chute physically empty.
  if (!beltIdle() || guide.moving ||
      (auxApi.phase != API_DONE && auxApi.phase != API_CANCELLED)) return false;
  auxApi.phase = API_IDLE;
  auxAuto.needsEmptyConfirmation = false;
  auxAuto.armed = false;
  LOG("AUX_API_OPERATOR_EMPTY_CONFIRMED: not sensor validation");
  // After CANCELLED, STOP_LATCH remains set until explicit resume.
  return true;
}

void auxUpdate() {
  // Nonblocking: MUST run regularly in the main Arduino loop().
  serviceBelt();
  serviceGuide();
  serviceAuto(); // Preserve existing autoroute behavior for serial testing.
  const uint32_t now = (uint32_t)millis();
  switch (auxApi.phase) {
    case API_IDLE:
    case API_PREPARED:
    case API_DONE:
    case API_CANCELLED: return;
    case API_PREPARING:
      if (stopLatched || elapsedMs(now, auxApi.phaseAt) >= AUTO_MAX_GUIDE_WAIT_MS) {
        LOG("AUX_API_FAULT: guide settle/command timeout"); auxCancel(); return;
      }
      if (!guide.moving && guide.known && guide.output == auxApi.target) {
        auxApi.phase = API_SETTLING;
        auxApi.phaseAt = now;
        LOG("AUX_API_GUIDE_COMMAND_REACHED: waiting 400ms");
      }
      return;
    case API_SETTLING:
      if (stopLatched || guide.moving || !beltIdle()) {
        LOG("AUX_API_FAULT: movement during settle"); auxCancel(); return;
      }
      if (elapsedMs(now, auxApi.phaseAt) >= AUTO_GUIDE_SETTLE_MS) {
        auxApi.phase = API_PREPARED;
        LOG("AUX_API_PREPARED: wait for main conveyor handoff and auxStart()");
      }
      return;
    case API_WAIT_OUTPUT:
      if (stopLatched) { auxCancel(); return; }
      if (belt.output == auxApi.runPwm && belt.pending == 0) {
        auxApi.phase = API_RUNNING;
        auxApi.startedAt = belt.startedAt;
        LOG("AUX_API_PWM_ON AT_MS=%lu", (unsigned long)auxApi.startedAt);
      } else if (elapsedMs(now, auxApi.phaseAt) >= AUTO_MAX_OUTPUT_WAIT_MS) {
        LOG("AUX_API_FAULT: output start timeout"); auxCancel();
      }
      return;
    case API_RUNNING:
      if (stopLatched || belt.output != auxApi.runPwm) {
        LOG("AUX_API_FAULT: output interrupted"); auxCancel(); return;
      }
      if (elapsedMs(now, auxApi.startedAt) >= auxAuto.runMs) {
        stopBelt();
        auxApi.phase = API_DONE;
        LOG("AUX_API_DONE: PWM OFF; physical exit UNKNOWN; operator verifyempty required");
      }
      return;
  }
}

// ===== MAIN CONVEYOR EVENT BRIDGE (ONE Arduino Mega) =====
// Do not infer product arrival from elapsed time. Main controller owns handoff event.
// All event calls MUST happen ONCE per product, not every loop iteration.
// Main must not allow a new product until an actual empty check is acknowledged.
bool mainAuxOnClassified(AuxRoute route) {
  if (!auxIsReady()) return false;
  return auxPrepare(route);  // guide only, auxiliary PWM stays OFF
}

bool mainAuxReadyForHandoff() {
  return auxIsPrepared(); // commanded servo position + settle; not measured position
}

bool mainAuxOnHandoffEvent() {
  // Call at the agreed upstream transfer point, only after ready.
  // If the product needs a moving belt to transfer, coordinate main release WITH this call.
  if (!mainAuxReadyForHandoff()) return false;
  return auxStart();
}

bool mainAuxMotorFinished() {
  return auxIsDone(); // PWM OFF after 8 s, NOT verified product arrival/exit
}

// Continuous integrated LIVE mode has a different completion contract from
// auxConfirmEmpty(): release a normally finished command sequence, without
// claiming an operator observation or measured physical exit. Cancellation
// always retains the existing explicit operator recovery requirement.
bool mainAuxReleaseCompletedSequence() {
  if (auxApi.phase != API_DONE || stopLatched || !beltIdle() || guide.moving ||
      auxAuto.phase != AUTO_IDLE || auxAuto.armed || !guide.known ||
      guide.output != auxApi.target) return false;
  auxApi.phase = API_IDLE;
  auxAuto.needsEmptyConfirmation = false;
  LOG("AUX_SEQUENCE_RELEASED PHYSICAL_EXIT=UNVERIFIED OPERATOR_CONFIRMED=0");
  return true;
}

bool mainAuxOperatorConfirmedEmpty() {
  // Never call automatically on a timer. Operator visually confirms empty.
  return auxConfirmEmpty();
}

void mainAuxEmergencyCancel() { auxCancel(); }

void formatAngle(int32_t value, char *out, size_t capacity) {
  if (value < 0) snprintf(out, capacity, "UNSET");
  else snprintf(out, capacity, "%ld.%03ld", (long)(value / 1000L), (long)(value % 1000L));
}
void apiStatus() {
  // Short, single high-priority diagnostic line to avoid Uno log queue overflow.
  LOG("AUX_API PHASE=%u READY=%u PREPARED=%u DONE=%u PWM=%d", 
      (unsigned int)auxApi.phase, (unsigned int)auxIsReady(),
      (unsigned int)auxIsPrepared(), (unsigned int)auxIsDone(), belt.output);
}
void status() {
  apiStatus();  // Put prepared/done first so it fits in the Uno log buffer.
  char target[12], output[12], center[12], left[12], right[12];
  formatAngle(guide.known ? guide.target : -1, target, sizeof(target));
  formatAngle(guide.known ? guide.output : -1, output, sizeof(output));
  formatAngle(positions[CENTER_SLOT], center, sizeof(center));
  formatAngle(positions[LEFT_SLOT], left, sizeof(left));
  formatAngle(positions[RIGHT_SLOT], right, sizeof(right));
  LOG("AUX OUTPUT_PWM=%d PENDING_PWM=%d RUN_MS=%lu STOP_LATCH=%u",
      belt.output, belt.pending, belt.output ? (unsigned long)elapsedMs((uint32_t)millis(), belt.startedAt) : 0UL,
      (unsigned int)stopLatched);
  LOG("G1 PIN=%d VALID=%u SIGNAL=%s TARGET_CMD=%s OUTPUT_CMD=%s MOVING_CMD=%u",
      AUX_GUIDE_PIN, (unsigned int)guideConfigValid(), guide.servo.attached() ? "ON" : "OFF",
      target, output, (unsigned int)guide.moving);
  LOG("SPEED_CMD_DPS=%u RANGE=%d..%d CENTER=%s LEFT=%s RIGHT=%s LOG_DROPPED=%lu",
      (unsigned int)speedDps, guide.minDeg, guide.maxDeg, center, left, right, (unsigned long)droppedLogs);
  LOG("PHYSICAL_POSITION/STOP/EXIT=UNMEASURED; PWM and command speed are not measurements");
  LOG("AUTO PHASE=%s ARM=%u EMPTY_UNVERIFIED=%u DEFECT_DIR=%d HOLD_DIR=%d PWM=%d RUN_MS=%lu",
      autoPhaseName(), (unsigned int)auxAuto.armed, (unsigned int)auxAuto.needsEmptyConfirmation,
      (int)auxAuto.defectDirection, (int)auxAuto.holdDirection,
      auxAuto.pwm, (unsigned long)auxAuto.runMs);
}
void help() {
  LOG("AUX_SINGLE_GUIDE_MANUAL v1: Mega / 115200 / Newline; no automatic motion");
  LOG("b128 / b 128 / b-128 / b0: AUX +/-PWM, range -192..192; 10s limit");
  LOG("g1 52 / g 52 / g1 +1 / g1 -5: one guide; absolute first");
  LOG("speed 60 / speed 150 / speed 0: command-deg/s 0..360; 0=direct target");
  LOG("range 0 180: narrow guide software limits; no automatic movement");
  LOG("setcenter / setleft / setright: save settled command TARGET in RAM");
  LOG("center / left / right: recall registered angle only; belt unchanged");
  LOG("stop / !: OFF + latch; resume: enable NEW commands only");
  LOG("detach: OFF + latch + remove guide signal; holding torque may disappear");
  LOG("status / p / help. Presets UNSET after restart; g2/m/s1/s2/route unsupported");
  LOG("Set guide while AUX stopped and empty; verify before manually starting belt");
  LOG("AUTO EXPERIMENTAL: autodir defect -1 / autodir hold 1 (physical +/- only)");
  LOG("autopwm 100 / autorunms 2500: BOTH require physical calibration first");
  LOG("autoarm / autodisarm / autoroute left|right|end|hold: guide->settle->PWM->OFF");
  LOG("verifyempty: OPERATOR CONFIRMS belt/chute physically empty; never sensor-proof");
  LOG("autoarm required before each legacy cycle; no AI label map or physical DONE");
  LOG("NEW API test: apiprepare left|right|end|hold -> wait PREPARED -> apistart -> verifyempty");
  LOG("NEW API test: apicancel on fault; after cancellation inspect, verifyempty, resume");
  LOG("MAIN mock events: mainclass left|right|end|hold, wait PREPARED, mainhandoff");
}
bool parseInteger(const char *text, long &value) {
  if (!text || !*text) return false;
  const char *p = text;
  if (*p == '+' || *p == '-') ++p;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') ++p;
  if (*p) return false;
  errno = 0; char *end = NULL;
  value = strtol(text, &end, 10);
  return errno != ERANGE && end && *end == '\0';
}
void savePosition(uint8_t slot) {
  if (!guide.known || guide.moving || !beltIdle()) {
    LOG("REJECT: register after absolute angle/ramp completion with AUX OFF"); return;
  }
  positions[slot] = guide.target;
  LOG("SAVED_RAM SLOT=%u: command angle only, not measured position", (unsigned int)slot);
}
void handleLine(char *line) {
  char *context = NULL;
  char *cmd = strtok_r(line, " \t", &context);
  if (!cmd) return;
  char *a = strtok_r(NULL, " \t", &context);
  char *b = strtok_r(NULL, " \t", &context);
  if (strtok_r(NULL, " \t", &context)) { inputError("TOO_MANY_ARGS"); return; }
  for (char *p = cmd; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
  LOG("REQUEST: %s %s %s", cmd, a ? a : "", b ? b : "");
  // Simulated MAIN EVENTS for isolated Uno bench test (not a physical arrival sensor).
  // Real Mega MAIN code calls mainAuxOnClassified/mainAuxOnHandoffEvent directly.
  if (!strcmp(cmd, "mainclass") && a && !b) {
    AuxRoute route;
    if (!strcmp(a, "left")) route = AUX_LEFT;
    else if (!strcmp(a, "right")) route = AUX_RIGHT;
    else if (!strcmp(a, "end")) route = AUX_END;
    else if (!strcmp(a, "hold")) route = AUX_HOLD;
    else { LOG("MAIN_REJECT: route left|right|end|hold"); return; }
    if (mainAuxOnClassified(route)) LOG("MAIN_CLASS_ACCEPT: guide moving; wait READY");
    else LOG("MAIN_REJECT: busy/not empty/stop latched");
    return;
  }
  if (!strcmp(cmd, "mainhandoff") && !a && !b) {
    if (mainAuxOnHandoffEvent()) LOG("MAIN_HANDOFF_ACCEPT: AUX start requested");
    else LOG("MAIN_REJECT: guide not ready or already started");
    return;
  }
  if (!strcmp(cmd, "maincancel") && !a && !b) { mainAuxEmergencyCancel(); return; }
  // Text fixtures for testing the SAME call-based API before MAIN integration.
  if (!strcmp(cmd, "apiprepare") && a && !b) {
    AuxRoute route;
    if (!strcmp(a,"left")) route = AUX_LEFT;
    else if (!strcmp(a,"right")) route = AUX_RIGHT;
    else if (!strcmp(a,"end")) route = AUX_END;
    else if (!strcmp(a,"hold")) route = AUX_HOLD;
    else { LOG("AUX_API_REJECT: expected left|right|end|hold"); return; }
    if (!auxPrepare(route)) LOG("AUX_API_REJECT: prepare failed");
    return;
  }
  if (!strcmp(cmd,"apistart") && !a && !b) {
    if (!auxStart()) LOG("AUX_API_REJECT: not prepared/busy");
    return;
  }
  if (!strcmp(cmd,"apicancel") && !a && !b) { auxCancel(); return; }
  // During/after AUTO or the new function API, block manual changes, except STOP.
  // In AUTO_AWAIT_EMPTY even resume does NOT authorize a new cycle.
  if (auxApi.phase != API_IDLE && strcmp(cmd, "status") && strcmp(cmd, "apistatus") && strcmp(cmd, "p") &&
      strcmp(cmd, "help") && strcmp(cmd, "stop") && strcmp(cmd, "detach") &&
      strcmp(cmd, "verifyempty") && strcmp(cmd, "resume")) {
    LOG("REJECT: function API busy/done; STOP or verifyempty first"); return;
  }
  if ((autoLocked() || auxAuto.armed) && strcmp(cmd, "status") && strcmp(cmd, "apistatus") && strcmp(cmd, "p") &&
      strcmp(cmd, "help") && strcmp(cmd, "stop") && strcmp(cmd, "detach") &&
      strcmp(cmd, "verifyempty") && strcmp(cmd, "resume") &&
      strcmp(cmd, "autoroute") && strcmp(cmd, "autodisarm")) {
    LOG("REJECT: AUTO in progress or empty unverified; STOP/verifyempty first"); return;
  }
  if (!a && !b) {
    if (!strcmp(cmd, "help")) { help(); return; }
    if (!strcmp(cmd, "apistatus")) { apiStatus(); return; }
    if (!strcmp(cmd, "status") || !strcmp(cmd, "p")) { status(); return; }
    if (!strcmp(cmd, "stop")) { if (auxApi.phase != API_IDLE) auxCancel(); else stopAll(false); return; }
    if (!strcmp(cmd, "detach")) { if (auxApi.phase != API_IDLE) auxCancel(); stopAll(true); return; }
    if (!strcmp(cmd, "verifyempty")) {
      if (!beltIdle() || guide.moving || (auxAuto.phase != AUTO_AWAIT_EMPTY &&
          auxAuto.phase != AUTO_IDLE)) {
        LOG("REJECT: not yet safe to verify; stop motion and inspect first"); return;
      }
      if (auxApi.phase != API_IDLE) {
        if (!auxConfirmEmpty()) {
          LOG("REJECT: function API still active; cannot verifyempty"); return;
        }
      } else {
        auxAuto.needsEmptyConfirmation = false;
      }
      auxAuto.phase = AUTO_IDLE;
      auxAuto.armed = false;
      LOG("OPERATOR_EMPTY_CONFIRMED: manual statement, NO SENSOR VALIDATION"); return;
    }
    if (!strcmp(cmd, "autodisarm")) {
      if (auxAuto.phase != AUTO_IDLE) {
        LOG("AUTO_REJECT: stop active cycle before disarming"); return;
      }
      auxAuto.armed = false;
      LOG("AUTO_DISARMED: no movement"); return;
    }
    if (!strcmp(cmd, "autoarm")) {
      if (stopLatched || autoLocked() || !beltIdle() || guide.moving ||
          !guideConfigValid() || !guide.known || !guide.servo.attached() ||
          !auxAuto.pwm || !auxAuto.runMs ||
          (!auxAuto.defectDirection && !auxAuto.holdDirection)) {
        LOG("AUTO_ARM_REJECT: STOP/guide/belt/empty state unresolved"); return;
      }
      auxAuto.armed = true;
      LOG("AUTO_ARMED: operator checked hardware and belt empty; NOT sensor verified"); return;
    }
    if (!strcmp(cmd, "resume")) {
      stopLatched = false; LOG("RESUMED: no old motion restarted; send NEW request"); return;
    }
    const char *saveNames[SLOT_COUNT] = {"setcenter", "setleft", "setright"};
    const char *goNames[SLOT_COUNT] = {"center", "left", "right"};
    for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
      if (!strcmp(cmd, saveNames[i])) { savePosition(i); return; }
      if (!strcmp(cmd, goNames[i])) {
        if (positions[i] < 0) { LOG("REJECT: position UNSET; register first"); return; }
        requestGuide(positions[i]); return;
      }
    }
  }
  long value = 0, high = 0;
  if (!strcmp(cmd, "autodir") && a && b) {
    if ((strcmp(a, "defect") && strcmp(a, "hold")) ||
        !parseInteger(b, value) || (value != -1 && value != 1) ||
        !beltIdle() || guide.moving || auxAuto.armed) {
      LOG("AUTO_REJECT: autodir defect|hold -1|1 with AUX stopped/unarmed"); return;
    }
    if (!strcmp(a, "defect")) auxAuto.defectDirection = (int8_t)value;
    else auxAuto.holdDirection = (int8_t)value;
    LOG("AUTO_DIR_SET %s=%ld ELECTRICAL_SIGN_ONLY", a, value); return;
  }
  if (!strcmp(cmd, "autopwm") && a && !b) {
    if (!parseInteger(a, value) || value < 1 || value > PWM_LIMIT || !beltIdle() || auxAuto.armed) {
      LOG("AUTO_REJECT: autopwm 1..192 with AUX stopped/unarmed"); return;
    }
    auxAuto.pwm = (int)value;
    LOG("AUTO_PWM_SET=%d; not verified physical speed", auxAuto.pwm); return;
  }
  if (!strcmp(cmd, "autorunms") && a && !b) {
    if (!parseInteger(a, value) || value < AUTO_MIN_RUN_MS ||
        value > AUTO_MAX_RUN_MS || !beltIdle() || auxAuto.armed) {
      LOG("AUTO_REJECT: autorunms 300..9500 with AUX stopped/unarmed"); return;
    }
    auxAuto.runMs = (uint32_t)value;
    LOG("AUTO_RUN_MS=%lu; PWM ON time, not delivery confirmation", (unsigned long)auxAuto.runMs);
    return;
  }
  if (!strcmp(cmd, "autoroute") && a && !b) {
    requestAutoRoute(a); return;
  }

  if (cmd[0] == 'b' && (cmd[1] == '\0' || cmd[1] == '+' || cmd[1] == '-' ||
                        (cmd[1] >= '0' && cmd[1] <= '9'))) {
    const char *number = cmd[1] ? cmd + 1 : a;
    if (b || (cmd[1] && a) || !parseInteger(number, value) ||
        value < -PWM_LIMIT || value > PWM_LIMIT) { inputError("BELT_VALUE"); return; }
    requestBelt((int)value); return;
  }
  if ((!strcmp(cmd, "g") || !strcmp(cmd, "g1")) && a && !b) {
    if (!parseInteger(a, value) || value < -180 || value > 180) { inputError("ANGLE_VALUE"); return; }
    const bool relative = *a == '+' || *a == '-';
    if (relative && !guide.known) { LOG("REJECT: physical start unknown; send absolute angle first"); return; }
    requestGuide((relative ? guide.target : 0) + (int32_t)value * 1000L); return;
  }
  if (!strcmp(cmd, "speed") && a && !b) {
    if (!parseInteger(a, value) || value < 0 || value > MAX_SPEED_DPS) { inputError("SPEED_VALUE"); return; }
    speedDps = (uint16_t)value; guide.lastTick = (uint32_t)millis();
    if (!stopLatched && guide.moving && speedDps == 0) {
      guide.output = guide.target; guide.moving = false; writeGuide();
    }
    LOG("SPEED_CMD_DPS=%u; not measured shaft speed", (unsigned int)speedDps); return;
  }
  if (!strcmp(cmd, "range") && a && b) {
    if (!parseInteger(a, value) || !parseInteger(b, high) || value < 0 || value >= high || high > 180) {
      inputError("RANGE_VALUE"); return;
    }
    if (!beltIdle() || guide.moving || (guide.known &&
        (guide.target < value * 1000L || guide.target > high * 1000L ||
         guide.output < value * 1000L || guide.output > high * 1000L))) {
      LOG("REJECT: stop motion and place command inside range, or detach first"); return;
    }
    guide.minDeg = (int)value; guide.maxDeg = (int)high;
    LOG("RANGE=%d..%d; no movement; out-of-range presets rejected when called", guide.minDeg, guide.maxDeg); return;
  }
  inputError("UNKNOWN_OR_MALFORMED_COMMAND");
}
void setup() {
  // IMPORTANT: never touch unused MAIN pins here. The integrated main sketch owns them.
  digitalWrite(AUX_IA, LOW); digitalWrite(AUX_IB, LOW);
  pinMode(AUX_IA, OUTPUT); pinMode(AUX_IB, OUTPUT);
  auxAuto.phase = AUTO_IDLE; auxAuto.armed = false;
  auxAuto.needsEmptyConfirmation = false;
  auxAuto.defectDirection = auxAuto.holdDirection = 0;
  auxAuto.pwm = 0; auxAuto.runMs = 0; auxAuto.target = 0;
  auxAuto.runPwm = 0; auxAuto.route = "none";
  auxAuto.phaseAt = auxAuto.runStartedAt = 0;
  belt.output = belt.pending = 0; belt.hasStopped = false;
  belt.startedAt = belt.stoppedAt = 0;
  writeBelt();
  guide.pinValid = guidePinAllowed(AUX_GUIDE_PIN);
  guide.minDeg = GUIDE_MIN_DEG; guide.maxDeg = GUIDE_MAX_DEG;
  guide.known = guide.moving = false; guide.target = guide.output = 0; guide.lastTick = 0;
  if (guide.pinValid) { digitalWrite((uint8_t)AUX_GUIDE_PIN, LOW); pinMode((uint8_t)AUX_GUIDE_PIN, OUTPUT); }
  // Servo remains detached; no home angle or automatic cycle on startup.
  Serial.begin(115200);
  auxLoadTestedConfiguration(); // Loads bench-proven RAM values; does NOT move hardware.
  if (!guideConfigValid()) LOG("CONFIG: guide disabled; set AUX_GUIDE_PIN only after actual wiring check");
  help(); status();
}
void loop() {
  auxUpdate();
  if (inputLength && elapsedMs((uint32_t)millis(), lastInputAt) >= LINE_TIMEOUT_MS) {
    inputLength = 0; discardLine = true; inputError("PARTIAL_LINE_TIMEOUT");
  }
  for (uint8_t n = 0; n < RX_BUDGET && Serial.available() > 0; ++n) {
    const uint8_t c = (uint8_t)Serial.read();
    if (c == '!') {
      if (auxApi.phase != API_IDLE) auxCancel(); else stopAll(false); inputLength = 0; discardLine = true;
    } else if (c == '\r' || c == '\n') {
      if (!discardLine && inputLength) { inputLine[inputLength] = '\0'; handleLine(inputLine); }
      inputLength = 0; discardLine = false;
    } else if (!discardLine) {
      if ((c < 32 && c != '\t') || c >= 127 || inputLength >= LINE_SIZE - 1) {
        inputLength = 0; discardLine = true; inputError("INVALID_OR_OVERLONG_LINE");
      } else { inputLine[inputLength++] = (char)c; lastInputAt = (uint32_t)millis(); }
    }
  }
  auxUpdate(); pumpLogs();
}
