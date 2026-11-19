// Jo Min-gyeong's D26 normally closed input monitor, adapted only at the
// maintained integration boundary. Existing main/aux motion algorithms stay intact.
bool estopLatched = false, estopEmptyVerified = false, estopWasOpen = false;
void pollEstop() {
  const bool open = digitalRead(MainAuxSettings::ESTOP_INPUT_PIN) != LOW;
  const bool newlyOpened = open && !estopWasOpen;
  if (newlyOpened) estopEmptyVerified = false;
  estopWasOpen = open;
  if (!open || (estopLatched && !newlyOpened)) return;
  estopLatched = true; estopEmptyVerified = false;
  haltAll(false, "ESTOP_INPUT_OPEN");
  LINK_LOG("ESTOP_LATCHED PIN=%u MAIN_AUX_PWM_OFF SOFTWARE_ONLY POWER_NOT_CUT",
           static_cast<unsigned>(MainAuxSettings::ESTOP_INPUT_PIN));
}
void beginEstop() {
#if defined(__AVR__)
  pinMode(MainAuxSettings::ESTOP_INPUT_PIN, INPUT_PULLUP);
#else
  // Host stubs model the external contact separately from the internal pull-up.
  pinMode(MainAuxSettings::ESTOP_INPUT_PIN, INPUT);
  digitalWrite(MainAuxSettings::ESTOP_INPUT_PIN, HIGH);
#endif
  estopLatched = estopEmptyVerified = estopWasOpen = false;
  pollEstop();
}
void printEstop() {
  LINK_LOG("ESTOP_PIN=%u NC_OPEN=%u LATCH=%u RESET=VERIFYEMPTY_THEN_RESUME",
           static_cast<unsigned>(MainAuxSettings::ESTOP_INPUT_PIN),
           digitalRead(MainAuxSettings::ESTOP_INPUT_PIN) != LOW, static_cast<unsigned>(estopLatched));
}
bool estopCommandAllowed(const char *cmd, uint8_t count) {
  if (!estopLatched) return true;
  if (count == 2 && !strcmp(cmd, "PING")) return true;
  return count == 1 && (!strcmp(cmd, "STATUS") || !strcmp(cmd, "LINK_STATUS") ||
      !strcmp(cmd, "HELP") || !strcmp(cmd, "STOP") || !strcmp(cmd, "DETACH") ||
      !strcmp(cmd, "BENCH") || !strcmp(cmd, "VERIFYEMPTY") || !strcmp(cmd, "RESUME"));
}
