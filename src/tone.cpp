#include <Arduino.h>
#include "tone.h"

#define BUZZER_CHANNEL 0
#define BUZZER_RESOLUTION 8
constexpr int BUZZER_PIN = 15;

void tone(int frequency, int duration) {
  ledcSetup(BUZZER_CHANNEL, frequency, BUZZER_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 127);
  delay(duration);
  ledcWrite(BUZZER_CHANNEL, 0);
}

void noTone() {
  ledcDetachPin(BUZZER_PIN);
}
