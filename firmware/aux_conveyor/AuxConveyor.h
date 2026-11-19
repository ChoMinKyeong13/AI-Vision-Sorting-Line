#ifndef AUX_CONVEYOR_H
#define AUX_CONVEYOR_H
#include <Arduino.h>

// Physical route destinations; main owner maps AI labels to routes.
enum AuxRoute : uint8_t { AUX_LEFT = 0, AUX_RIGHT = 1, AUX_END = 2, AUX_HOLD = 3 };

void auxLoadTestedConfiguration(); // No outputs, called by aux sketch setup().
bool auxPrepare(AuxRoute route);    // Request guide movement, belt OFF.
bool auxIsPrepared();              // Guide command and 400 ms settle finished.
bool auxStart();                   // Request auxiliary PWM start.
void auxUpdate();                  // Nonblocking; call in every main loop().
bool auxIsDone();                  // Commanded 8 s PWM run complete; NOT physical exit.
bool auxIsReady();                 // New-cycle ready, operator empty confirmation cleared.
void auxCancel();                  // Stop, latch and require empty check.
bool auxConfirmEmpty();            // OPERATOR ONLY, after physical belt/chute check.
#endif
