// Data acquisition (Arduino UNO, ATmega328P): sets the controller's mode, logs
// which LED is on for every frame, and records an external TTL (e.g. trial start).
//
// Commands accepted over serial (from Bonsai), one character each, upper or lower case:
//
//   Command   Mode (VBG)   LEDs, cycled one per frame
//   '0'       000          OFF
//   'V'       100          410 only
//   'B'       010          470 only
//   'G'       001          565 only
//   'Y'       110          410 / 470
//   'M'       101          410 / 565
//   'C'       011          470 / 565
//   'X'       111          410 / 470 / 565
//
//   Anything else (e.g. \r \n from the line terminator) is ignored.
//   Requires the revised controller sketch; the original one only handled 110 and 111.
//
// Log line, one per frame (at the LED onset):
//   V,B,G,TTL,mode,FLAG                     (default, same format as before)
//   V,B,G,TTL,mode,FLAG,t_onset,t_ttl        (with LOG_TIMESTAMP = true)
//
//   TTL    = 1 if the external TTL is HIGH now OR rose at any time since the previous
//            logged frame. A level signal reads 1 for every frame of the trial, as before;
//            a short pulse can no longer be missed (it shows on the next frame's line).
//   t_onset = micros() at this frame's LED onset
//   t_ttl   = micros() of the TTL rising edge reported on this line, 0 if none
//
// NOTE: opening the serial port resets the UNO. For ~1-2 s the bootloader runs and
// any command sent in that window is lost. Have Bonsai wait ~2 s after opening the
// port before sending 'X' / 'Y'.

#include <Arduino.h>

// Mode at power-up / reset. OFF means nothing is triggered until Bonsai asks for it.
// Set to 110 to restore the old behaviour (acquisition starts as soon as the UNO boots).
const int START_MODE = 0;

// Adds t_onset and t_ttl columns. Leave false to keep the 6-column format Bonsai parses now.
const bool LOG_TIMESTAMP = true;

// OUTPUT PINS (mode-select lines to the controller Nano).
// D10, D11, D12 are PB2, PB3, PB4 on the ATmega328P; writeModePins() sets them in one write.
const uint8_t PINMODE_V = 10;
const uint8_t PINMODE_B = 11;
const uint8_t PINMODE_G = 12;

// INPUT PINS
// V/B/G tap the Nano's LED410/LED470/LED565 driver lines (ground truth for which LED is on).
const uint8_t V   = 4;
const uint8_t B   = 5;
const uint8_t G   = 6;
const uint8_t TTL = 8;  // external TTL; D8 = PB0 = PCINT0 (pin-change interrupt)

int mode = START_MODE;
int prevV = 0, prevB = 0, prevG = 0;
int FLAG = 0;  // 0-99, wraps; one tick per logged frame

// Set by the interrupt, cleared when reported in a log line
volatile bool ttlRose = false;
volatile unsigned long ttlRiseUs = 0;

// Pin-change interrupt for D8: fires on both edges, we keep only rising ones.
// Catches pulses of any length, even while the main loop is busy printing.
ISR(PCINT0_vect) {
  if (PINB & _BV(PB0)) {
    if (!ttlRose) ttlRiseUs = micros();  // keep the first rise since the last report
    ttlRose = true;
  }
}

// Set all three mode lines simultaneously, so the controller can never read a
// half-changed state (e.g. 100 while going from 000 to 110).
void writeModePins(int m) {
  uint8_t bits = 0;
  if (m / 100 % 10) bits |= _BV(PB2);  // V -> D10
  if (m / 10 % 10)  bits |= _BV(PB3);  // B -> D11
  if (m % 10)       bits |= _BV(PB4);  // G -> D12
  uint8_t mask = _BV(PB2) | _BV(PB3) | _BV(PB4);

  uint8_t sreg = SREG;
  cli();
  PORTB = (PORTB & ~mask) | bits;
  SREG = sreg;
}

void setup() {
  // 250000 baud divides 16 MHz exactly (0% error). Bonsai must use the same rate.
  Serial.begin(250000);

  // Set the output levels BEFORE making the pins outputs, so they never glitch.
  writeModePins(mode);
  pinMode(PINMODE_V, OUTPUT);
  pinMode(PINMODE_B, OUTPUT);
  pinMode(PINMODE_G, OUTPUT);

  pinMode(V, INPUT);
  pinMode(B, INPUT);
  pinMode(G, INPUT);
  pinMode(TTL, INPUT);  // add a 10k pull-down so an unplugged cable reads LOW

  // Enable the pin-change interrupt on D8 only
  PCMSK0 = _BV(PCINT0);
  PCIFR  = _BV(PCIF0);   // clear any stale flag
  PCICR |= _BV(PCIE0);
}

void loop() {
  // Non-blocking command handling: one byte per pass, pins are polled every pass.
  if (Serial.available()) {
    int c = Serial.read();
    int newMode = mode;
    // Modes are stored as decimal digits VBG. Don't write 011 or 001 here:
    // a leading 0 makes C++ read the number as octal (011 == 9).
    switch (toupper(c)) {
      case '0': newMode = 0;   break;  // OFF
      case 'V': newMode = 100; break;  // 410
      case 'B': newMode = 10;  break;  // 470
      case 'G': newMode = 1;   break;  // 565
      case 'Y': newMode = 110; break;  // 410 / 470
      case 'M': newMode = 101; break;  // 410 / 565
      case 'C': newMode = 11;  break;  // 470 / 565
      case 'X': newMode = 111; break;  // 410 / 470 / 565
      default:  break;                 // ignore \r, \n, stray bytes
    }
    if (newMode != mode) {
      if (mode == 0) {
        // Starting acquisition: forget TTL edges that happened while OFF,
        // so they aren't attributed to the first frame of the new run.
        uint8_t sreg = SREG;
        cli();
        ttlRose = false;
        SREG = sreg;
      }
      mode = newMode;
      writeModePins(mode);
    }
  }

  int valV = digitalRead(V);
  int valB = digitalRead(B);
  int valG = digitalRead(G);

  // Onset of a frame = rising edge on any LED line
  if ((valV && !prevV) || (valB && !prevB) || (valG && !prevG)) {
    unsigned long tOnset = micros();

    // Take and clear the latched TTL edge atomically
    uint8_t sreg = SREG;
    cli();
    bool rose = ttlRose;
    unsigned long tRise = ttlRiseUs;
    ttlRose = false;
    SREG = sreg;

    int valTTL = (digitalRead(TTL) == HIGH || rose) ? 1 : 0;

    Serial.print(valV);
    Serial.print(',');
    Serial.print(valB);
    Serial.print(',');
    Serial.print(valG);
    Serial.print(',');
    Serial.print(valTTL);
    Serial.print(',');
    // Commanded mode, zero-padded to 3 digits (e.g. 011); may lead the LEDs by one frame at a switch
    if (mode < 100) Serial.print('0');
    if (mode < 10)  Serial.print('0');
    Serial.print(mode);
    Serial.print(',');
    if (FLAG < 10) Serial.print('0');
    Serial.print(FLAG);
    if (LOG_TIMESTAMP) {
      Serial.print(',');
      Serial.print(tOnset);
      Serial.print(',');
      Serial.print(rose ? tRise : 0UL);
    }
    Serial.println();

    FLAG = (FLAG + 1) % 100;
  }

  prevV = valV;
  prevB = valB;
  prevG = valG;
}
