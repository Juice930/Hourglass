
// ================== Digital Hourglass for ESP32 ==================
// Hardware assumed: ESP32 + MPU6050 + 2x MAX7219 8x8 LED matrices (daisy-chained)
// Libraries (install via Library Manager): 
//   - MD_MAX72XX by MajicDesigns
//   - Adafruit MPU6050
//   - Adafruit Unified Sensor
//   - Wire
//
// CONFIG MODE ACCESS: Only accessible when turning on or after shake cancellation
// - At startup: Automatically enters CONFIG mode for time selection
// - From any state: Shake to cancel and enter CONFIG mode
// - From STANDBY: Tilt to start countdown directly (no CONFIG access)
//
// LEDC Configuration Notes:
// - Using 6-bit resolution instead of 8-bit to avoid frequency conflicts
// - Fixed frequency at 400Hz for maximum compatibility
// - Different tones achieved by varying duty cycle instead of frequency
// - This completely avoids "frequency and duty resolution cannot be achieved" errors

#include <MD_MAX72xx.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// -------- MAX7219 CONFIG --------
// Choose your module type. Most generic 8x8 MAX7219 boards work with FC16_HW.
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
// VSPI pins on ESP32 (default): SCK=18, MOSI=23. Choose any free CS pin:
const uint8_t PIN_CS = 5;
const uint16_t NUM_DEVICES = 2;  // 2 x 8x8
MD_MAX72XX mx(HARDWARE_TYPE, PIN_CS, NUM_DEVICES);

// -------- Buzzer (ESP32 LEDC) --------
const int BUZZER_PIN = 4;       // Change if needed
const int BUZZER_CHANNEL = 0;    // LEDC channel 0..15
const int BUZZER_RESOLUTION = 6; // 6-bit resolution (reduced from 8 to avoid frequency conflicts)
bool melodyPlayed = false;

static void buzzerTone(unsigned int freq, unsigned int durationMs) {
  // Safety checks
  if (freq == 0 || freq > 2000) { // Valid musical frequencies are typically 20Hz-20kHz, but we'll limit to 2kHz
    Serial.print("Invalid frequency detected: ");
    Serial.print(freq);
    Serial.println("Hz - skipping tone");
    ledcWrite(BUZZER_CHANNEL, 0);
    delay(durationMs);
    return;
  }
  
  if (durationMs > 1000) { // Sanity check on duration
    Serial.print("Invalid duration detected: ");
    Serial.print(durationMs);
    Serial.println("ms - using 100ms");
    durationMs = 100;
  }
  
  // Map frequency to duty cycle to create different tone qualities
  // Higher frequencies get higher duty cycles for brighter sound
  // Increased duty cycles for louder volume
  uint32_t duty;
  if (freq >= 600) {
    duty = 56; // High duty for high frequencies (F5, E5, D5) - increased from 48
  } else if (freq >= 500) {
    duty = 48; // Medium-high duty for medium frequencies (C5, B4) - increased from 40
  } else if (freq >= 400) {
    duty = 40; // Medium duty for lower frequencies (A4, G4) - increased from 32
  } else {
    duty = 32; // Lower duty for lowest frequencies (F4) - increased from 24
  }
  
  // Debug: Print what we're doing
  Serial.print("Playing tone: freq=");
  Serial.print(freq);
  Serial.print("Hz, duty=");
  Serial.print(duty);
  Serial.print("/63, duration=");
  Serial.print(durationMs);
  Serial.println("ms");
  
  ledcWrite(BUZZER_CHANNEL, duty);
  delay(durationMs);
}

static void playCompletionMelody() {
  // A pleasant, gentle completion jingle
  // Notes: C4, D4, E4, F4, G4, F4, E4, D4, C4 (ascending then descending)
  const unsigned int notes[] = { 
    262, 294, 330, 349, 392, 349, 330, 294, 262
  };
  // Durations: Gentle, flowing rhythm
  const unsigned int lens[] = { 
    300, 200, 200, 200, 400, 200, 200, 200, 500
  };
  const size_t count = sizeof(notes)/sizeof(notes[0]);
  
  Serial.print("Playing completion melody with ");
  Serial.print(count);
  Serial.println(" notes");
  
  for (size_t i = 0; i < count; i++) {
    if (notes[i] == 0) break; // Safety check
    if (i < sizeof(lens)/sizeof(lens[0])) { // Ensure we don't read beyond lens array
      buzzerTone(notes[i], lens[i]);
    } else {
      buzzerTone(notes[i], 200); // Default duration if lens array is shorter
    }
  }
  
  // Ensure buzzer is off
  ledcWrite(BUZZER_CHANNEL, 0);
  Serial.println("Melody playback completed");
}

// -------- MPU6050 --------
Adafruit_MPU6050 mpu;

// -------- System States --------
enum SystemState {
  STANDBY,      // Waiting for tilt to start
  CONFIG,       // Configuration menu for time selection
  COUNTDOWN,    // Sand is flowing
  PAUSED,       // Laid on side; animation paused
  COMPLETED     // Song finished, waiting for tilt to restart
};
SystemState currentState = STANDBY;

// -------- Simulation grid --------
// 16 rows (two 8x8), 8 columns
const uint8_t H = 16;
const uint8_t W = 8;
bool grid[H][W];
bool nextGrid[H][W];
// Track unchanged steps for auto-flip
uint8_t unchangedSteps = 0;
const uint8_t UNCHANGED_STEPS_THRESHOLD = 10; // Number of steps before auto-flip

uint32_t lastStep = 0;
const uint16_t STEP_MS = 500;     // animation speed (grid debug/unused timing)

// Orientation memory tracks the last stable orientation
// This ensures consistent animation direction regardless of current tilt

const uint8_t DIAMOND_GRAINS = 25; // For 8x8 diamond
struct Grain { int y, x; };
Grain diamondGrains[DIAMOND_GRAINS];
Grain diamondGrainsMirror[DIAMOND_GRAINS];
uint8_t activeGrains = 0; // How many grains are currently animated
bool negativePhase = false; // If true, show mirrored grains

// Debounce flipping
uint32_t lastFlipMs = 0;
const uint16_t FLIP_DEBOUNCE_MS = 800;

// Neck column (0..7); you can move it or even oscillate it for fun
const uint8_t NECK_X = 4;

// Amount of sand (0..H*W). Start with the "bottom" half full-ish:
uint16_t SAND_COUNT = 16; // Reduced for debugging

// ---------------------- Calibration ----------------------
// Actual measured frame count from the animation: 262 frames
// This gives us accurate timing for the sand flow animation
uint32_t TARGET_DURATION_MS = 5000; // Made variable for configuration
#define TOTAL_FALL_STEPS 262
uint16_t FALL_STEP_MS = 0; // Will be calculated based on selected time

// Independent bottom falling-grain animation
uint8_t bottomFallPhase = 0;       // 0..7 along the bottom diagonal

// Global animation timer to support pause/resume without catch-up
uint32_t lastAnimTickMs = 0;

// Frame counter to measure actual total steps
uint32_t totalFramesCounted = 0;
bool frameCountingStarted = false;
bool frameCountingCompleted = false;

// Sideways hysteresis tracking
bool lastIsSideways = false;
uint32_t sidewaysStateChangeMs = 0;

// Orientation memory - tracks the last stable orientation
bool lastStableWasFlipped = false;

// Shake detection variables
float lastAccelMagnitude = 0;
uint32_t lastShakeCheck = 0;
const uint16_t SHAKE_CHECK_INTERVAL = 50; // Check shake every 50ms
const float SHAKE_THRESHOLD = 15.0; // m/s² threshold for shake detection
const uint8_t SHAKE_DEBOUNCE_MS = 500; // Debounce shake detection
uint32_t lastShakeDetected = 0;

// Track if user has been in stable position since completion (no longer needed)
// bool hasBeenInStablePositionSinceCompletion = false;

// -------- Configuration Menu --------
struct TimeOption {
  uint16_t value;
  char unit;
  const char* label;
};

const TimeOption TIME_OPTIONS[] = {
    {30, 'S', "30S"},
    {2, 'M', "2M"},
    {1, 'M', "1M"},
    {3, 'M', "3M"},
    {5, 'M', "5M"},
    {10, 'M', "10M"}
};

const uint8_t NUM_TIME_OPTIONS = sizeof(TIME_OPTIONS) / sizeof(TIME_OPTIONS[0]);
uint8_t selectedTimeOption = 0;

// Digit switching variables for multi-digit display
uint8_t digitIndex = 0;
uint32_t lastDigitChange = 0;

// ---------------------- Function Declarations ----------------------
void showConfigMenu();
void updateConfigSelection(float rollAngle, float pitchAngle);
void confirmTimeSelection();
void displayDigit(uint8_t digit, uint8_t matrix);
void displayNumber(uint16_t number, uint8_t matrix);
void displayLetter(uint8_t letterIndex, uint8_t matrix);

// ---------------------- Helpers ----------------------

void render() {
  // Don't clear the display - preserve the demo content
  // Only update the sand animation areas without clearing everything
  
  // Our logical (0,0) is the top-left of the combined 16x8.
  // Device 0 = top matrix, device 1 = bottom matrix.
  // For daisy-chained devices, we calculate the absolute column position
  for (uint8_t y=0; y<H; y++) {
    uint8_t dev = (y < 8) ? 0 : 1;
    uint8_t row = (y < 8) ? y : (y - 8);

    for (uint8_t x=0; x<W; x++) {
      bool on = grid[y][x];
      // Calculate absolute column: device 0 uses cols 0-7, device 1 uses cols 8-15
      uint8_t absCol = dev * 8 + x;
      mx.setPoint(row, absCol, on);
    }
  }
}

void clearGrid() {
  for (uint8_t y=0; y<H; y++)
    for (uint8_t x=0; x<W; x++)
      grid[y][x] = false;
}

const uint8_t N = 8; // 8x8 matrix
const uint8_t GRAINS = 64;
uint8_t grainOrder[GRAINS]; // Diagonal order
uint8_t fallingGrain = 0; // Current grain count transferred
bool topFilled = true; // If true, top is filled, grains are falling

// Encapsulated debug print
void printGridDebug() {
  // Grid debug commented out
  /*
  // Serial debug for grid and sand count commented out
  // uint16_t sandInBottom = 0;
  // uint8_t yStart = (gravityDir == +1) ? 8 : 0;
  // uint8_t yEnd   = (gravityDir == +1) ? 16 : 8;
  // for (uint8_t y = yStart; y < yEnd; y++) {
  //   for (uint8_t x = 0; x < W; x++) {
  //     if (grid[y][x]) sandInBottom++;
  //   }
  // }
  // Serial.print("Sand in bottom/top: ");
  // Serial.println(sandInBottom);
  // Serial.println("Grid:");
  // for (uint8_t y = 0; y < H; y++) {
  //   for (uint8_t x = 0; x < W; x++) {
  //     Serial.print(grid[y][x] ? "#" : ".");
  //   }
  //   Serial.println();
  // }
  // Print detected tilt instead
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float xAccel = a.acceleration.x;
  float yAccel = a.acceleration.y;
  float zAccel = a.acceleration.z;
  float magnitude = sqrt(xAccel*xAccel + yAccel*yAccel + zAccel*zAccel);
  float tiltAngle = 0;
  if (magnitude > 0.1) {
    tiltAngle = acos(abs(zAccel) / magnitude) * 180.0 / PI;
  }
  Serial.print("Tilt: ");
  Serial.print(tiltAngle, 1);
  Serial.print("° | Z: ");
  Serial.print(zAccel, 2);
  Serial.println(" m/s²");
  */
  
  // Print system state info (overwrite same line)
  Serial.print(" | State: ");
  switch(currentState) {
    case STANDBY: Serial.print("STANDBY"); break;
    case CONFIG: Serial.print("CONFIG"); break;
    case COUNTDOWN: Serial.print("COUNTDOWN"); break;
    case PAUSED: Serial.print("PAUSED"); break;
    case COMPLETED: Serial.print("COMPLETED"); break;
  }
  Serial.print(" | Sand: ");
  Serial.print(fallingGrain);
  Serial.print("    "); // Add spaces to clear any remaining characters
}

// Encapsulated unchanged grid check
bool isGridUnchanged() {
  for (uint8_t y = 0; y < H; y++) {
    for (uint8_t x = 0; x < W; x++) {
      if (grid[y][x] != nextGrid[y][x]) {
        return false;
      }
    }
  }
  return true;
}

// Shake detection function
bool detectShake(float currentMagnitude) {
  uint32_t now = millis();
  
  // Only check shake at regular intervals
  if (now - lastShakeCheck < SHAKE_CHECK_INTERVAL) {
    return false;
  }
  lastShakeCheck = now;
  
  // Calculate acceleration change
  float accelChange = abs(currentMagnitude - lastAccelMagnitude);
  lastAccelMagnitude = currentMagnitude;
  
  // Check if acceleration change exceeds threshold and debounce time has passed
  if (accelChange > SHAKE_THRESHOLD && (now - lastShakeDetected) > SHAKE_DEBOUNCE_MS) {
    lastShakeDetected = now;
    return true;
  }
  
  return false;
}

// Fill grainOrder with diagonal pattern (top right to bottom left)
void initGrainOrder() {
  uint8_t idx = 0;
  for (int d = 0; d <= 2*(N-1); d++) {
    for (int y = 0; y < N; y++) {
      int x = d - y;
      if (x >= 0 && x < N) {
        grainOrder[idx++] = y * N + x;
      }
    }
  }
}

void seedSandBottom() {
  clearGrid();
  // Seed sand in a diamond pattern
  // Use orientation memory to determine which half to fill
  
  uint16_t placed = 0;
  for (int y = 0; y < H && placed < SAND_COUNT; y++) {
    for (int x = 0; x < W && placed < SAND_COUNT; x++) {
      // Diamond pattern: |x - 3.5| + |y - center_y| <= 3.5
      float centerY = (H-1) / 2.0;
      float centerX = (W-1) / 2.0;
      if (abs(x - centerX) + abs(y - centerY) <= (W-1)/2.0) {
        // Fill based on orientation memory
        if (lastStableWasFlipped) {
          // Flipped: fill top half (rows 0-7) - this will be the "bottom" in flipped world
          if (y < 8) {
            grid[y][x] = true;
            placed++;
          }
        } else {
          // Upright: fill bottom half (rows 8-15) - this is the bottom in normal world
          if (y >= 8) {
            grid[y][x] = true;
            placed++;
          }
        }
      }
    }
  }
}

inline bool inBounds(int y, int x) {
  return (y >= 0 && y < (int)H && x >= 0 && x < (int)W);
}

void initDiamondMapping() {
  uint8_t idx = 0;
  int cy = H/2, cx = W/2;
  for (int layer = 0; layer <= 4; layer++) {
    for (int dy = -layer; dy <= layer; dy++) {
      int dx = layer - abs(dy);
      int y = cy + dy;
      int x1 = cx + dx;
      int x2 = cx - dx;
      if (idx < DIAMOND_GRAINS && inBounds(y, x1)) {
        diamondGrains[idx].y = y;
        diamondGrains[idx].x = x1;
        diamondGrainsMirror[idx].y = H-1-y;
        diamondGrainsMirror[idx].x = W-1-x1;
        idx++;
      }
      if (dx != 0 && idx < DIAMOND_GRAINS && inBounds(y, x2)) {
        diamondGrains[idx].y = y;
        diamondGrains[idx].x = x2;
        diamondGrainsMirror[idx].y = H-1-y;
        diamondGrainsMirror[idx].x = W-1-x2;
        idx++;
      }
    }
  }
}

// Reset system to start state
void resetSystem() {
  currentState = COUNTDOWN;
  topFilled = true;
  fallingGrain = 0;
  bottomFallPhase = 0;
  unchangedSteps = 0;
  melodyPlayed = false;
  // hasBeenInStablePositionSinceCompletion = false; // Reset stable position flag (no longer needed)
  
  // Reset frame counter for new countdown
  frameCountingStarted = false;
  frameCountingCompleted = false;
  totalFramesCounted = 0;
  
  clearGrid();
  Serial.println("System reset - countdown started!");
}

// -------- Configuration Menu Functions --------
void showConfigMenu() {
  // Don't clear the entire display - let the demo continue running
  
  // Top screen shows the number
  const TimeOption& option = TIME_OPTIONS[selectedTimeOption];
  Serial.println("=== CONFIGURATION MENU ===");
  Serial.print("Selected: ");
  Serial.print(option.value);
  Serial.print(" ");
  Serial.println(option.unit);
  Serial.print("Option ");
  Serial.print(selectedTimeOption + 1);
  Serial.print(" of ");
  Serial.println(NUM_TIME_OPTIONS);
  Serial.println("========================");
  
  // Display the time value on the top matrix
  if (option.value < 10) {
    // Single digit: just show the digit
    displayDigit(option.value, 0);
  } else {
    // Multiple digits: show current digit based on global state
    uint8_t tensDigit = option.value / 10;
    uint8_t onesDigit = option.value % 10;
    
    // Display the current digit
    displayDigit(digitIndex == 0 ? tensDigit : onesDigit, 0);
  }
  
  // Display the unit on the bottom matrix
  if (option.unit == 'S') {
    displayLetter(0, 1); // S
  } else if (option.unit == 'M') {
    displayLetter(1, 1); // M
  }
}

// Digit patterns for 8x8 matrices (0 = off, 1 = on)
// Each pattern is stored as 8 rows of 8 bits
static const uint8_t digitPatterns[10][8] = {
  // 0
  {0x00, 0x38, 0x44, 0x42, 0x22, 0x1C, 0x00, 0x00},
  // 1
  {0x00, 0x70, 0x20, 0x10, 0x08, 0x04, 0x00, 0x00},
  // 2
  {0x00, 0x30, 0x40, 0x3C, 0x02, 0x04, 0x08, 0x00},
  // 3
  {0x00, 0x10, 0x20, 0x48, 0x72, 0x14, 0x18, 0x00},
  // 4
  {0x00, 0x10, 0x08, 0x48, 0x30, 0x10, 0x08, 0x00},
  // 5
  {0x00, 0x10, 0x28, 0x4C, 0x11, 0x12, 0x0C, 0x00},
  // 6
  {0x00, 0x18, 0x24, 0x46, 0x09, 0x12, 0x1C, 0x00},
  // 7
  {0x00, 0x10, 0x20, 0x40, 0x78, 0x04, 0x00, 0x00},
  // 8
  {0x00, 0x30, 0x48, 0x4C, 0x32, 0x12, 0x0C, 0x00},
  // 9
  {0x00, 0x30, 0x48, 0x30, 0x10, 0x08, 0x04, 0x00}
};

// Letter patterns for 8x8 matrices
static const uint8_t letterPatterns[2][8] = {
  // S
  {0x00, 0x30, 0x48, 0x10, 0x12, 0x0C, 0x00, 0x00},
  // M
  {0x00, 0x10, 0x18, 0x74, 0x22, 0x10, 0x08, 0x00}
};

void displayDigit(uint8_t digit, uint8_t matrix) {
  if (digit < 0 || digit > 9) return; // Support digits 0-9
  
  const uint8_t* pattern = digitPatterns[digit]; // No need to convert to 0-based index
  
  // Clear only the specific matrix area
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      // For matrix 0: use cols 0-7, for matrix 1: use cols 8-15
      uint8_t col = x + (matrix * 8);
      mx.setPoint(y, col, false); // Clear this matrix area
    }
  }
  
  // Display the digit pattern on the specified matrix
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      if (pattern[y] & (0x80 >> x)) {
        // For matrix 0: use cols 0-7, for matrix 1: use cols 8-15
        uint8_t col = x + (matrix * 8);
        mx.setPoint(y, col, true);
      }
    }
  }
}

void displayNumber(uint16_t number, uint8_t matrix) {
  if (number >= 0 && number <= 9) {
    displayDigit(number, matrix);
  }
}

void displayLetter(uint8_t letterIndex, uint8_t matrix) {
  if (letterIndex >= 2) return; // Only support S (0) and M (1)
  
  const uint8_t* pattern = letterPatterns[letterIndex];
  
  // Clear only the specific matrix area
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      // For matrix 0: use cols 0-7, for matrix 1: use cols 8-15
      uint8_t col = x + (matrix * 8);
      mx.setPoint(y, col, false); // Clear this matrix area
    }
  }
  
  // Display the letter pattern on the specified matrix
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      if (pattern[y] & (0x80 >> x)) {
        // For matrix 0: use cols 0-7, for matrix 1: use cols 8-15
        uint8_t col = x + (matrix * 8);
        mx.setPoint(y, col, true);
      }
    }
  }
}

void updateConfigSelection(float rollAngle, float pitchAngle) {
  // Roll axis: browse through time options
  // Left roll (negative) = shorter time, Right roll (positive) = longer time
  // Pitch axis: confirm selection (pitch up 30°+ = confirm)
  
  // Handle roll-based browsing
    float normalizedRoll;
    uint8_t newSelection;
    
    if (rollAngle < -25)
        rollAngle = -25;
    else if(rollAngle > 34.5)
        rollAngle = 34.5;

    newSelection = (uint8_t) ((rollAngle + 25) / 10);

    Serial.print("Roll: ");
    Serial.print(rollAngle, 1);
    Serial.print("° → Selection: ");
    Serial.println(newSelection);
    
    if (newSelection != selectedTimeOption) {
      selectedTimeOption = newSelection;
      showConfigMenu();
    }  
  // Handle pitch-based confirmation
  if (pitchAngle >= 30.0) {
    // Confirm selection and start countdown
    Serial.print("Confirming selection at pitch ");
    Serial.print(pitchAngle, 1);
    Serial.println("°");
    confirmTimeSelection();
  }
}

void confirmTimeSelection() {
  const TimeOption& selected = TIME_OPTIONS[selectedTimeOption];
  
  // Calculate the target duration based on selection
  if (selected.unit == 'S') {
    TARGET_DURATION_MS = selected.value * 1000; // Convert seconds to milliseconds
  } else {
    TARGET_DURATION_MS = selected.value * 60000; // Convert minutes to milliseconds
  }
  
  // Recalculate fall step timing to match the selected duration
  // Each fall step should take TARGET_DURATION_MS / TOTAL_FALL_STEPS milliseconds
  FALL_STEP_MS = TARGET_DURATION_MS / TOTAL_FALL_STEPS;
  
  Serial.print("Time selected: ");
  Serial.print(selected.value);
  Serial.print(selected.unit);
  Serial.print(" (");
  Serial.print(TARGET_DURATION_MS);
  Serial.print("ms) → Step time: ");
  Serial.print(FALL_STEP_MS);
  Serial.println("ms");
  
  // Exit config mode and start countdown
  currentState = COUNTDOWN;
  resetSystem();
}

// TODO: Add logGridState function here when needed


// Encapsulated falling/transfer draw (does not advance counts)
static void drawSandState() {
  clearGrid();
  
  // Use orientation memory instead of measuring tilt angle
  // This ensures consistent animation direction based on last stable position
  
  if (topFilled) {
    if (lastStableWasFlipped) {
      // Flipped orientation: fill the top 8x8 matrix (rows 0-7)
      for (uint8_t y = 0; y < N; y++) {
        for (uint8_t x = 0; x < N; x++) {
          grid[y][x] = true;
        }
      }
    } else {
      // Upright orientation: fill the bottom 8x8 matrix (rows 8-15)
      for (uint8_t y = 8; y < 16; y++) {
        for (uint8_t x = 0; x < W; x++) {
          grid[y][x] = true;
        }
      }
    }
  } else {
    if (lastStableWasFlipped) {
      // Flipped orientation: sand flows from bottom matrix (rows 8-15) to top matrix (rows 0-7)
      // Fill bottom matrix (source) except for grains that have fallen
      for (uint8_t i = fallingGrain; i < GRAINS; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = (N-1-idx/N) + 8; // Mirror Y in bottom matrix
        uint8_t x = (N-1-idx%N);      // Mirror X for 180° rotation
        grid[y][x] = true;
      }
      // Fill top matrix (destination) with grains that have fallen
      for (uint8_t i = 0; i < fallingGrain; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = idx / N;       // Keep Y as-is in top matrix
        uint8_t x = idx % N;       // Keep X as-is in top matrix
        grid[y][x] = true;
      }
    } else {
      // Upright orientation: sand flows from top matrix (rows 0-7) to bottom matrix (rows 8-15)
      // Fill top matrix (source) except for grains that have fallen
      for (uint8_t i = fallingGrain; i < GRAINS; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = idx / N;
        uint8_t x = idx % N;
        grid[y][x] = true;
      }
      // Fill bottom matrix (destination) with grains that have fallen (bottom up, X mirrored)
      for (uint8_t i = 0; i < fallingGrain; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = (N-1-idx/N) + 8; // Mirror Y in bottom matrix
        uint8_t x = (N-1-idx%N);      // Mirror X for 180° rotation
        grid[y][x] = true;
      }
    }
  }
}

// Overlay the single falling grain on the appropriate diagonal
static void drawBottomFallingGrain() {
  // Use orientation memory instead of measuring tilt angle
  // This ensures consistent animation direction based on last stable position
  
  uint8_t y, x;
  
  if (lastStableWasFlipped) {
    // Flipped orientation: falling grain in top matrix, moving from bottom-right to top-left
    y = 7 - bottomFallPhase;
    x = 7 - bottomFallPhase;
    
    // Check if we've hit the sand surface (stop falling when we hit existing grains)
    if (y < 8 && x < 8 && grid[y][x]) {
      return; // Don't draw falling grain if it would overlap with existing sand
    }
  } else {
    // Upright orientation: falling grain in bottom matrix, moving from top-left to bottom-right
    y = 8 + bottomFallPhase;
    x = bottomFallPhase;
    
    // Check if we've hit the sand surface (stop falling when we hit existing grains)
    if (y < 16 && x < 8 && grid[y][x]) {
      return; // Don't draw falling grain if it would overlap with existing sand
    }
  }
  
  // Only draw the falling grain if it's within bounds and not overlapping
  if (y >= 0 && y < H && x >= 0 && x < W) {
    grid[y][x] = true;
  }
}

// Advance the bottom falling grain animation; return true if it just touched ground
static bool advanceBottomFallingGrain() {
  // Frame counting logic
  if (!frameCountingCompleted) {
    if (!frameCountingStarted) {
      frameCountingStarted = true;
      totalFramesCounted = 0;
      Serial.println("*** FRAME COUNTING STARTED ***");
    }
    totalFramesCounted++;
  }
  
  // Check if the next position would hit the sand surface
  uint8_t nextY, nextX;
  bool wouldHitSurface = false;
  
  if (lastStableWasFlipped) {
    // Flipped orientation: check next position in top matrix
    nextY = 7 - ((bottomFallPhase + 1) % 8);
    nextX = 7 - ((bottomFallPhase + 1) % 8);
    if (nextY < 8 && nextX < 8 && grid[nextY][nextX]) {
      wouldHitSurface = true;
    }
  } else {
    // Upright orientation: check next position in bottom matrix
    nextY = 8 + ((bottomFallPhase + 1) % 8);
    nextX = (bottomFallPhase + 1) % 8;
    if (nextY < 16 && nextX < 8 && grid[nextY][nextX]) {
      wouldHitSurface = true;
    }
  }
  
  // Check if current position is at the edge of the matrix
  bool atEdge = (bottomFallPhase == 7);
  
  // Grain touches ground when it hits the surface OR reaches the edge
  bool touchedGround = wouldHitSurface || atEdge;
  
  if (touchedGround) {
    // Immediately reset to start the next grain falling
    bottomFallPhase = 0;
  } else {
    // Continue with current grain
    bottomFallPhase = (bottomFallPhase + 1) % 8;
  }
  
  return touchedGround;
}

// Perform one sand transfer step (advance one grain from top to bottom)
static void performSandStep() {
  if (topFilled) {
    // Start transfer on first ground touch
    topFilled = false;
    fallingGrain = 0;
  } else if (fallingGrain < GRAINS) {
    fallingGrain++;
  }
}

void stepSand() {
  // No pause: transfer only advances when falling grain touches ground
  // This function only draws current state; stepping is handled externally
  drawSandState();
}

void updateSandLogic() {
  // Don't update anything if paused
  if (currentState == PAUSED)
    return;
  
  // Only run sand animation when in COUNTDOWN state
  if (currentState != COUNTDOWN)
    return;
  
  // Draw sand state
  stepSand();
  // Overlay independent falling grain
  drawBottomFallingGrain();
  // Render frame
  render();
  // Removed: printGridDebug(); to avoid breaking single-line output

  // Auto-flip logic when bottom is full and stable
  if (isGridUnchanged()) {
    unchangedSteps++;
  } else {
    unchangedSteps = 0;
  }
  uint16_t sandInBottom = 0;
  // Use orientation memory to determine which half contains the "bottom" (source of sand)
  uint8_t yStart, yEnd;
  if (lastStableWasFlipped) {
    // Flipped: "bottom" (source) is the top matrix (rows 0-7), "top" (destination) is bottom matrix (rows 8-15)
    yStart = 0;
    yEnd = 8;
  } else {
    // Upright: "bottom" (source) is the bottom matrix (rows 8-15), "top" (destination) is top matrix (rows 0-7)
    yStart = 8;
    yEnd = 16;
  }
  for (uint8_t y = yStart; y < yEnd; y++) {
    for (uint8_t x = 0; x < W; x++) {
      if (grid[y][x]) sandInBottom++;
    }
  }
  if (sandInBottom == SAND_COUNT && unchangedSteps >= UNCHANGED_STEPS_THRESHOLD) {
    // Auto-flip: switch orientation and reseed sand
    lastStableWasFlipped = !lastStableWasFlipped;
    seedSandBottom();
    unchangedSteps = 0;
    Serial.println();
    Serial.print("*** AUTO-FLIP: Orientation changed to ");
    Serial.print(lastStableWasFlipped ? "FLIPPED" : "UPRIGHT");
    Serial.println(" ***");
  }
}

// Check accelerometer for tilt detection
void checkTilt() {
  static SystemState lastRenderedState = STANDBY;
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float xAccel = a.acceleration.x;
  float yAccel = a.acceleration.y;
  float zAccel = a.acceleration.z;
  float magnitude = sqrt(xAccel*xAccel + yAccel*yAccel + zAccel*zAccel);
  
  // Calculate roll and pitch angles from accelerometer data
  // Roll: rotation around X-axis (left/right tilt like airplane banking)
  // Pitch: rotation around Y-axis (forward/backward tilt like airplane nose up/down)
  float rollAngle = atan2(yAccel, zAccel) * 180.0 / PI;
  float pitchAngle = atan2(-xAccel, sqrt(yAccel*yAccel + zAccel*zAccel)) * 180.0 / PI;
  
  // Calculate tilt angle for backward compatibility
  float tiltAngle = 0;
  if (magnitude > 0.1) {
    float cosAngle = zAccel / magnitude;
    tiltAngle = acos(abs(cosAngle)) * 180.0 / PI;
  }

  // Improved tilt detection with clear, non-overlapping zones
  // 0°-20°: UPRIGHT (stable) | 20°-70°: TILTED (unstable) | 70°-120°: SIDEWAYS (pause)
  // Zone detection uses orientation memory: both orientations use the same angle ranges
  // Both UPRIGHT and FLIPPED are considered stable positions for system behavior
  // Countdown cancellation is now handled by shake detection instead of angle zones
  
  // Use orientation memory to determine which angle to use for zone detection
  // Both orientations now use the same tilt angle ranges for consistent behavior
  float  effectiveTiltAngle = tiltAngle;
  
  // Detect upside down by checking if Z-acceleration is negative (pointing down)
  bool isUpsideDown = (zAccel < 0);
  
  // Both orientations now use the same tilt angle ranges
  // effectiveTiltAngle = tiltAngle for both UPRIGHT and FLIPPED orientations
  
  // Zone detection logic: both orientations use the same logic
  // - Upright orientation: 0°-20° is upright (stable)
  // - Flipped orientation: 0°-20° is upright (stable) - same angle ranges
  bool isUpright = (effectiveTiltAngle < 20.0);
  
  // Debug: Show the actual values being used for zone detection
  // Serial.print(" | isUpright:"); Serial.print(isUpright ? "YES" : "NO");
  // Serial.print(" | effectiveTilt:"); Serial.print(effectiveTiltAngle, 1);
  // Serial.print(" | stableCalc:"); Serial.print(lastStableWasFlipped ? "FLIP<20" : "UPRT<20");
  
  bool isSideways = (effectiveTiltAngle > 70.0 && effectiveTiltAngle < 120.0); // 70° to 120°: sideways (extended)
  
  // Debug: Show both raw and effective angles for troubleshooting
  // if (lastStableWasFlipped) {
  //   Serial.print(" | Raw: "); Serial.print(tiltAngle, 1); Serial.print("° → Effective: "); Serial.print(effectiveTiltAngle, 1); Serial.print("° (FLIP)");
  // }
  
                             // Stability detection: both orientations use the same logic
     // - Upright orientation: effectiveTiltAngle < 20.0 is stable
     // - Flipped orientation: effectiveTiltAngle < 20.0 is stable (same angle ranges)
     bool isStable = (effectiveTiltAngle < 20.0) || isUpsideDown;
  

  
  // Reset orientation memory when switching between stable positions
  // Update when we're actually in a stable position (UPRIGHT or FLIPPED)
  if (isStable) {
    // Check if we need to update orientation memory
    bool shouldBeFlipped = isUpsideDown;
    if (lastStableWasFlipped != shouldBeFlipped) {
      lastStableWasFlipped = shouldBeFlipped;
      Serial.println(); // New line for orientation change
      Serial.print("*** ORIENTATION MEMORY CHANGED: ");
      Serial.print(lastStableWasFlipped ? "FLIPPED" : "UPRIGHT");
      Serial.println(" ***");
    }
    
    // Show when entering stable mode
    static bool wasStable = false;
    if (!wasStable) {
      Serial.println();
      Serial.print("*** ENTERING STABLE MODE: ");
      Serial.print(lastStableWasFlipped ? "FLIPPED" : "UPRIGHT");
      Serial.println(" ***");
      wasStable = true;
      
             // Show entering stable mode message
       if (currentState == COMPLETED) {
         Serial.println("*** READY TO RESTART - Return to stable position ***");
       }
    }
  } else {
    // Reset stable flag when leaving stable mode
    static bool wasStable = false;
    wasStable = false;
  }


  
  uint32_t now = millis();


  
  if (isSideways != lastIsSideways) {
    lastIsSideways = isSideways;
    sidewaysStateChangeMs = now;
  }
  bool sidewaysStable = (now - sidewaysStateChangeMs >= 150); // Reduced from 200ms to 150ms for faster response

  // Serial.print("\rTilt: ");
  // Serial.print(tiltAngle, 1);
  // Serial.print("°");
  // if (lastStableWasFlipped) {
  //   Serial.print(" [FLIP] → effective:");
  //   Serial.print(effectiveTiltAngle, 1);
  //   Serial.print("°");
  // }
  // Serial.print(" | Z: ");
  // Serial.print(zAccel, 2);
  // Serial.print(" | Zones: ");
  // Serial.print(isUpright ? "UPRIGHT" : (isSideways ? "SIDEWAYS" : "TILTED"));
  // Serial.print(" | Mem:");
  // Serial.print(lastStableWasFlipped ? "FLIP" : "UPRT");
  // Serial.print(" | State: ");
  // switch(currentState) {
  //   case STANDBY: Serial.print("STANDBY"); break;
  //   case CONFIG: Serial.print("CONFIG"); break;
  //   case COUNTDOWN: Serial.print("COUNTDOWN"); break;
  //   case PAUSED: Serial.print("PAUSED"); break;
  //   case COMPLETED: Serial.print("COMPLETED"); break;
  // }
  // Serial.print(" | Stable:");
  // Serial.print(isStable ? "YES" : "NO");
  // Serial.print(" | UpsideDown:");
  // Serial.print(isUpsideDown ? "YES" : "NO");
  
  // Add shake detection debug (but don't call detectShake here to avoid double-calling)
  // Shake detection is handled in the state machine logic below

  // Check for state changes and render accordingly
  if (currentState != lastRenderedState) {
    lastRenderedState = currentState;
    if (currentState == PAUSED) {
      // When entering PAUSED state, render once and keep it frozen
      render();
      Serial.println("Display frozen in PAUSED state");
    }
  }

  if (currentState == STANDBY) {
    if (isSideways && sidewaysStable) {
      currentState = PAUSED;
      lastAnimTickMs = now;
      Serial.println(" | PAUSED");
      Serial.println("*** PAUSED STATE TRIGGERED FROM STANDBY ***");
    } else if (effectiveTiltAngle >= 20.0 && effectiveTiltAngle <= 70.0) {
      // Start countdown directly when tilting in STANDBY state
      // CONFIG mode is only accessible at startup or after shake cancellation
      Serial.println("*** STARTING COUNTDOWN ***");
      currentState = COUNTDOWN;
      resetSystem();
    } else if (isStable) {
      //Serial.println(" | Stable position");
    } else {
      //Serial.println(" | Other position");
    }
  } else if (currentState == CONFIG) {
    // Handle configuration menu
    if (detectShake(magnitude)) {
      // Shake to cancel configuration
      currentState = STANDBY;
      Serial.println(" | CONFIG CANCELLED - Shake detected!");
      Serial.println("*** CONFIG CANCELLED - Shake detected! ***");
    } else {
      // Update selection based on roll and pitch angles
      updateConfigSelection(rollAngle, pitchAngle);
    }
  } else if (currentState == COUNTDOWN) {
    // Check for shake to cancel countdown
    if (detectShake(magnitude)) {
      currentState = CONFIG;
      selectedTimeOption = 0;
      showConfigMenu();
      Serial.println(" | COUNTDOWN CANCELLED - Entering CONFIG mode!");
      Serial.println("*** COUNTDOWN CANCELLED - Entering CONFIG mode! ***");
    }
    // Allow pausing from COUNTDOWN state when laid sideways
    else if (isSideways && sidewaysStable) {
      currentState = PAUSED;
      lastAnimTickMs = now;
      Serial.println(" | PAUSED");
      Serial.println("*** PAUSED STATE TRIGGERED FROM COUNTDOWN ***");
    } else {
      //Serial.println(); // Just add newline for COUNTDOWN state
    }
  } else if (currentState == PAUSED) {
    // Check for shake to cancel from paused state
    if (detectShake(magnitude)) {
      currentState = CONFIG;
      selectedTimeOption = 0;
      showConfigMenu();
      Serial.println(" | CANCELLED FROM PAUSED - Entering CONFIG mode!");
      Serial.println("*** CANCELLED FROM PAUSED - Entering CONFIG mode! ***");
    } else if (!isSideways && sidewaysStable) {
      currentState = COUNTDOWN;
      lastAnimTickMs = now;
      Serial.println(" | RESUME COUNTDOWN");
      Serial.println("*** RESUMING FROM PAUSED STATE ***");
      Serial.println("Display active - countdown resumed");
    } else {
      Serial.println(" | PAUSED");
    }
  } else if (currentState == COMPLETED) {
    // Check for shake to reset to standby
    if (detectShake(magnitude)) {
      currentState = CONFIG;
      selectedTimeOption = 0;
      showConfigMenu();
      Serial.println(" | RESET - Entering CONFIG mode!");
      Serial.println("*** RESET - Entering CONFIG mode! ***");
    }
    else if (isSideways && sidewaysStable) {
      currentState = PAUSED;
      lastAnimTickMs = now;
      Serial.println(" | PAUSED");
      Serial.println("*** PAUSED STATE TRIGGERED FROM COMPLETED ***");
    } else if (isStable) {
      // When reaching stable position from COMPLETED, transition to STANDBY
      // This allows the user to restart countdown from STANDBY state
      currentState = STANDBY;
      topFilled = true;
      fallingGrain = 0;
      bottomFallPhase = 0;
      unchangedSteps = 0;
      melodyPlayed = false;
      clearGrid();
      seedSandBottom();
      Serial.println(" | Transitioning to STANDBY - Ready to restart countdown!");
      Serial.println("*** COMPLETED → STANDBY - Tilt to start new countdown ***");
    } else {
      Serial.println(" | Return to stable position to restart countdown");
    }
  }
}

// ---------------------- Setup & Loop ----------------------
void setup() {
  Wire.begin(); // SDA=21, SCL=22 on ESP32 by default
  Serial.begin(115200);

  // MAX7219
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 6); // 0..15
  // Don't clear here - let the demo start immediately

  // Buzzer
  ledcSetup(BUZZER_CHANNEL, 400, BUZZER_RESOLUTION); // Conservative frequency that works reliably
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0);

  // MPU6050
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found! Running without tilt detection.");
    // If no sensor, start countdown immediately
    resetSystem();
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 initialized. Tilt to start countdown.");
    Serial.println("Shake to cancel countdown from any state.");
  }

  // Start with upright orientation. Seed initial sand.
  lastStableWasFlipped = false;
  
  // Initialize shake detection
  lastAccelMagnitude = 0;
  lastShakeCheck = 0;
  lastShakeDetected = 0;
  
  initGrainOrder();
  seedSandBottom(); // Uncommented to initialize sand state
  
  // Start in configuration mode to select time
  currentState = CONFIG;
  selectedTimeOption = 0;
  showConfigMenu();
  Serial.println("Entering configuration mode. ROLL left/right to browse time options, PITCH up 30°+ to confirm.");
  
  // Clear display once at startup
  mx.clear();
  Serial.println("System initialized and ready.");
}

void loop() {
  static uint32_t lastTiltCheck = 0;
  uint32_t now = millis();

  // Check tilt every 100ms
  if (now - lastTiltCheck >= 100) {
    lastTiltCheck = now;
    checkTilt();
  }

  // If paused, don't do anything - keep display frozen
  if (currentState == PAUSED) {
    return; // Exit early, don't advance anything
  }

  // Only run countdown when active
  if (currentState == COUNTDOWN && now - lastAnimTickMs >= FALL_STEP_MS) {
    lastAnimTickMs = now;
    bool touched = advanceBottomFallingGrain();
      if (touched) {
        performSandStep();
        if (!topFilled && fallingGrain >= GRAINS && !melodyPlayed) {
          // Frame counting completed
          if (!frameCountingCompleted) {
            frameCountingCompleted = true;
            Serial.print("*** FRAME COUNTING COMPLETED: ");
            Serial.print(totalFramesCounted);
            Serial.println(" total frames ***");
          }
          
          melodyPlayed = true;
          currentState = COMPLETED;
          Serial.println("Starting completion melody...");
          playCompletionMelody();
          Serial.println("Melody finished. Countdown completed! Tilt again to restart.");
        }
      }
    updateSandLogic();
  }

  // Handle standby state
  if (currentState == STANDBY) {
    // Don't clear display - let demo continue running
    delay(100);
  }
  
  // Handle configuration state
  if (currentState == CONFIG) {
    // Handle digit switching for multi-digit display
    uint32_t now = millis();
    if (now - lastDigitChange >= 1000) { // Change every second
      lastDigitChange = now;
      digitIndex = (digitIndex + 1) % 2; // Alternate between 0 and 1
      
      // Update the display with the new digit
      const TimeOption& option = TIME_OPTIONS[selectedTimeOption];
      if (option.value >= 10) {
        uint8_t tensDigit = option.value / 10;
        uint8_t onesDigit = option.value % 10;
        displayDigit(digitIndex == 0 ? tensDigit : onesDigit, 0);
      }
    }
    
    delay(100);
  }
  

  

}
