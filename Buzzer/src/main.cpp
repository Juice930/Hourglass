#include <Arduino.h>

// Buzzer connected to pin 4
const int BUZZER_PIN = 4;
// 440 Hz is the musical note A4
const int FREQUENCY = 440;

void setup() {
  // Initialize serial communication for debugging
  Serial.begin(115200);
  
  // Set buzzer pin as output
  pinMode(BUZZER_PIN, OUTPUT);
  
  Serial.println("Buzzer initialized on pin 4");
  Serial.println("Playing 440 Hz note (A4)");
  
  // Start playing the 440 Hz note continuously
  tone(BUZZER_PIN, FREQUENCY);
}

void loop() {
  // Empty loop - tone continues playing from setup()
}