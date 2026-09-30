// Controller (Arduino Nano): camera trigger + LED sequencing.
// Mode is set by the acquisition Arduino (UNO) on D10/D11/D12.
//
// Mode pins (V B G):  000 OFF, 100 V, 010 B, 001 G, 110 VB, 101 VG, 011 BG, 111 VBG
// Enabled LEDs fire one per frame in the order 410 -> 470 -> 565, skipping disabled ones.

#include <Arduino.h>

// ---------------- Timing ----------------
const unsigned long FPS             = 40;                 // Hz (total, all channels)
const unsigned long FRAME_PERIOD_US = 1000000UL / FPS;    // 25000 us at 40 Hz
const unsigned long DARK_TIME_US    = 500;                // LEDs off + trigger HIGH between frames
const unsigned long T_EXCITATION_US = FRAME_PERIOD_US - DARK_TIME_US;
// DARK_TIME_US must leave room for the end-of-frame writes and the mode read (~250 us worst case).

// ---------------- Pins ----------------
const uint8_t CAM   = 2;   // Camera trigger, idle HIGH, LOW during exposure (Line 3 in SpinView)
const uint8_t LED_PINS[3]  = {4, 5, 6};    // 410, 470, 565 nm drivers
const uint8_t MODE_PINS[3] = {10, 11, 12}; // V, B, G (inputs from the UNO)

// ---------------- State ----------------
uint8_t mode       = 0;     // bitmask: bit0 = V, bit1 = B, bit2 = G
uint8_t ledIndex   = 0;     // next LED to try (0 = 410, 1 = 470, 2 = 565)
bool    running    = false;
unsigned long tStart = 0;   // scheduled start of the current frame

uint8_t readModePins() {
  uint8_t m = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (digitalRead(MODE_PINS[i]) == HIGH) m |= (1 << i);
  }
  return m;
}

// Accept a new mode only if it reads the same twice, 200 us apart.
// This rejects the in-between states seen while the UNO is changing its pins one at a time.
uint8_t readModeStable() {
  uint8_t m = readModePins();
  if (m != mode) {
    delayMicroseconds(200);
    if (readModePins() != m) return mode;  // still changing; keep the old mode this frame
  }
  return m;
}

uint8_t nextLed() {
  // mode is nonzero here, so this always finds an enabled LED
  while (!(mode & (1 << ledIndex))) ledIndex = (ledIndex + 1) % 3;
  uint8_t led = ledIndex;
  ledIndex = (ledIndex + 1) % 3;
  return led;
}

void setup() {
  // Set the level BEFORE making the pin an output, so CAM never drops LOW at boot
  // (pinMode(OUTPUT) alone drives LOW first, which is a falling edge = spurious trigger).
  digitalWrite(CAM, HIGH);
  pinMode(CAM, OUTPUT);

  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(LED_PINS[i], LOW);
    pinMode(LED_PINS[i], OUTPUT);
    pinMode(MODE_PINS[i], INPUT);  // add external pull-downs (e.g. 10k) so these don't float
  }
}

void loop() {
  // This runs in the dark period after the previous frame (or continuously while OFF)
  uint8_t newMode = readModeStable();
  if (newMode != mode) {
    mode = newMode;
    ledIndex = 0;  // restart the sequence at the first enabled LED
  }

  if (mode == 0) {
    running = false;
    return;
  }

  if (!running) {
    // First frame of a run: start now
    running = true;
    tStart = micros();
  } else {
    // Fixed schedule: next frame starts exactly one period after the last one,
    // so loop overhead doesn't accumulate as drift over long recordings.
    while (micros() - tStart < FRAME_PERIOD_US) {}
    tStart += FRAME_PERIOD_US;
  }

  uint8_t led = nextLed();

  digitalWrite(CAM, LOW);
  digitalWrite(LED_PINS[led], HIGH);

  while (micros() - tStart < T_EXCITATION_US) {}

  digitalWrite(CAM, HIGH);
  digitalWrite(LED_PINS[led], LOW);
}
