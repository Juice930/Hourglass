
// ================== Digital Hourglass for ESP32 ==================
// Hardware assumed: ESP32 + MPU6050 + 2x MAX7219 8x8 LED matrices (daisy-chained)
// Libraries (install via Library Manager): 
//   - MD_MAX72XX by MajicDesigns
//   - Adafruit MPU6050
//   - Adafruit Unified Sensor
//   - Wire
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
  // "Dancing in September" inspired jingle from Earth, Wind & Fire
  // Melody captures the descending pattern: "Dan-cing in Sep-tem-ber"
  // Notes: F5, E5, D5, C5, B4, A4, G4, F4
  const unsigned int notes[] = { 
    698, 659, 587, 523, 494, 440, 392, 349
  };
  // Durations: Mimic the rhythm of "Dan-cing in Sep-tem-ber"
  // "Dan" (long), "cing" (short), "in" (short), "Sep" (medium), "tem" (short), "ber" (long), "G4" (short), "F4" (long)
  const unsigned int lens[] = { 
    200, 100, 100, 150, 100, 200, 100, 200
  };
  const size_t count = sizeof(notes)/sizeof(notes[0]);
  
  Serial.print("Playing melody with ");
  Serial.print(count);
  Serial.println(" notes");
  
  for (size_t i = 0; i < count; i++) {
    if (notes[i] == 0) break; // Safety check
    if (i < sizeof(lens)/sizeof(lens[0])) { // Ensure we don't read beyond lens array
      buzzerTone(notes[i], lens[i]);
    } else {
      buzzerTone(notes[i], 100); // Default duration if lens array is shorter
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

// Orientation: +1 = gravity to increasing row index (top->bottom)
//              -1 = gravity upwards (bottom->top)
int gravityDir = +1;

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
// With current behavior: first bottom touch starts transfer (no increment),
// then 64 increments occur. Each increment happens every 8 fall steps.
// Total fall steps = 8 * (64 + 1) = 520.
#define TARGET_DURATION_MS 5000
#define TOTAL_FALL_STEPS 520
const uint16_t FALL_STEP_MS = (TARGET_DURATION_MS + (TOTAL_FALL_STEPS/2)) / TOTAL_FALL_STEPS; // rounded

// Independent bottom falling-grain animation
uint8_t bottomFallPhase = 0;       // 0..7 along the bottom diagonal

// Global animation timer to support pause/resume without catch-up
uint32_t lastAnimTickMs = 0;

// Sideways hysteresis tracking
bool lastIsSideways = false;
uint32_t sidewaysStateChangeMs = 0;

// Orientation memory - tracks the last stable orientation
bool lastStableWasFlipped = false;





// ---------------------- Helpers ----------------------

void render() {
  // Clear all modules
  mx.clear();

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
  // Seed sand in a rotated diamond (touches corners)
  uint16_t placed = 0;
  for (int y = 0; y < H && placed < SAND_COUNT; y++) {
    for (int x = 0; x < W && placed < SAND_COUNT; x++) {
      // Rotated diamond: |x - 3.5| == |y - 7.5| for the diagonals, <= 3.5 for inside
      if (abs((x - (W-1)/2.0) - (y - (H-1)/2.0)) <= (W-1)/2.0 && abs((x - (W-1)/2.0) + (y - (H-1)/2.0)) <= (W-1)/2.0) {
        // For bottom half, only fill lower triangle
        if ((gravityDir == +1 && y > x) || (gravityDir == -1 && y < x)) {
          grid[y][x] = true;
          placed++;
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
  clearGrid();
  Serial.println("System reset - countdown started!");
}



// Encapsulated falling/transfer draw (does not advance counts)
static void drawSandState() {
  clearGrid();
  
  // Check if hourglass is upside down using tilt angle (consistent with main logic)
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float magnitude = sqrt(a.acceleration.x*a.acceleration.x + a.acceleration.y*a.acceleration.y + a.acceleration.z*a.acceleration.z);
  float tiltAngle = 0;
  if (magnitude > 0.1) {
    float cosAngle = a.acceleration.z / magnitude;
    if (cosAngle >= 0) {
      tiltAngle = acos(cosAngle) * 180.0 / PI;
    } else {
      tiltAngle = 180.0 - acos(-cosAngle) * 180.0 / PI;
    }
  }
  bool isUpsideDown = (tiltAngle > 165.0);
  
  if (topFilled) {
    if (isUpsideDown) {
      // Upside down: fill the bottom 8x8 matrix
      for (uint8_t y = 8; y < 16; y++) {
        for (uint8_t x = 0; x < W; x++) {
          grid[y][x] = true;
        }
      }
    } else {
      // Right side up: fill the top 8x8 matrix
      for (uint8_t y = 0; y < N; y++) {
        for (uint8_t x = 0; x < N; x++) {
          grid[y][x] = true;
        }
      }
    }
  } else {
    if (isUpsideDown) {
      // Upside down: sand flows from bottom to top with 180° rotation
      // Fill bottom except for grains that have fallen
      for (uint8_t i = fallingGrain; i < GRAINS; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = (N-1-idx/N) + 8; // Mirror Y in bottom matrix
        uint8_t x = (N-1-idx%N);      // Mirror X for 180° rotation
        grid[y][x] = true;
      }
      // Fill top with grains that have fallen (top down, X and Y not mirrored)
      for (uint8_t i = 0; i < fallingGrain; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = idx / N;       // Keep Y as-is (no mirror)
        uint8_t x = idx % N;       // Keep X as-is (no mirror)
        grid[y][x] = true;
      }
    } else {
      // Right side up: sand flows from top to bottom (original behavior)
      // Fill top except for grains that have fallen
      for (uint8_t i = fallingGrain; i < GRAINS; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = idx / N;
        uint8_t x = idx % N;
        grid[y][x] = true;
      }
      // Fill bottom with grains that have fallen (bottom up, X mirrored)
      for (uint8_t i = 0; i < fallingGrain; i++) {
        uint8_t idx = grainOrder[i];
        uint8_t y = idx / N;
        uint8_t x = idx % N;
        grid[(N-1-y)+N][N-1-x] = true;
      }
    }
  }
}

// Overlay the single falling grain on the appropriate diagonal
static void drawBottomFallingGrain() {
  // Check if hourglass is upside down using tilt angle (consistent with main logic)
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float magnitude = sqrt(a.acceleration.x*a.acceleration.x + a.acceleration.y*a.acceleration.y + a.acceleration.z*a.acceleration.z);
  float tiltAngle = 0;
  if (magnitude > 0.1) {
    float cosAngle = a.acceleration.z / magnitude;
    if (cosAngle >= 0) {
      tiltAngle = acos(cosAngle) * 180.0 / PI;
    } else {
      tiltAngle = 180.0 - acos(-cosAngle) * 180.0 / PI;
    }
  }
  bool isUpsideDown = (tiltAngle > 165.0);
  
  uint8_t y, x;
  
  if (isUpsideDown) {
    // Upside down: falling grain in top matrix, moving from bottom-right to top-left
    y = 7 - bottomFallPhase;
    x = 7 - bottomFallPhase;
  } else {
    // Right side up: falling grain in bottom matrix, moving from top-left to bottom-right
    y = 8 + bottomFallPhase;
    x = bottomFallPhase;
  }
  
  grid[y][x] = true;
}

// Advance the bottom falling grain animation; return true if it just touched ground
static bool advanceBottomFallingGrain() {
  bool touchedGround = (bottomFallPhase == 7);
  bottomFallPhase = (bottomFallPhase + 1) % 8;
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
  uint8_t yStart = (gravityDir == +1) ? 8 : 0;
  uint8_t yEnd   = (gravityDir == +1) ? 16 : 8;
  for (uint8_t y = yStart; y < yEnd; y++) {
    for (uint8_t x = 0; x < W; x++) {
      if (grid[y][x]) sandInBottom++;
    }
  }
  if (sandInBottom == SAND_COUNT && unchangedSteps >= UNCHANGED_STEPS_THRESHOLD) {
    gravityDir = -gravityDir;
    seedSandBottom();
    unchangedSteps = 0;
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
  float tiltAngle = 0;
  if (magnitude > 0.1) {
    // Calculate tilt angle from vertical (0° = upright, 180° = upside down)
    // Use the Z-axis as the reference for vertical orientation
    float cosAngle = zAccel / magnitude;
    
    // Handle angles beyond 90° by considering the sign of Z-acceleration
    if (cosAngle >= 0) {
      // 0° to 90°: upright to horizontal
      tiltAngle = acos(cosAngle) * 180.0 / PI;
    } else {
      // 90° to 180°: horizontal to upside down
      tiltAngle = 180.0 - acos(-cosAngle) * 180.0 / PI;
    }
  }

  // Improved tilt detection with clear, non-overlapping zones
  // 0°-15°: UPRIGHT (stable) | 15°-75°: TILTED (unstable) | 75°-120°: SIDEWAYS (pause) | 120°-165°: CANCELED | 165°-180°: FLIPPED (stable)
  // Zone detection uses orientation memory: if last stable was FLIPPED, use complement angle; if UPRIGHT, use normal angle
  // CANCELED zone (120°-165°) cancels countdown and waits for UPRIGHT or FLIPPED position
  // Both UPRIGHT and FLIPPED are considered stable positions for system behavior
  
  // Use orientation memory to determine whether to use normal or complement angle
  // If last stable was FLIPPED, use complement angle; if UPRIGHT, use normal angle
  // Orientation memory only updates when actually reaching stable positions (UPRIGHT or FLIPPED)
  float effectiveTiltAngle = tiltAngle;
  
  bool isUpsideDown = (tiltAngle > 165.0);                        // 165° to 180°: upside down (raw angle)
  
  // Determine which angle to use for zone detection based on last stable orientation
  if (lastStableWasFlipped) {
    // Last stable was FLIPPED - use complement angle for consistent zone behavior
    // This means: when tilted 10° from vertical, effectiveTiltAngle = 170° (which should be treated as "upright")
    effectiveTiltAngle = 180.0 - tiltAngle;
  }
  // If last stable was UPRIGHT, use normal angle (effectiveTiltAngle = tiltAngle)
  
  // Now calculate zones using the appropriate angle
  // When last stable was FLIPPED: effectiveTiltAngle = 180° - tiltAngle (so 165° becomes 15°, 170° becomes 10°, etc.)
  // When last stable was UPRIGHT: effectiveTiltAngle = tiltAngle (normal behavior)
  bool isUpright;
  if (lastStableWasFlipped) {
    // In flipped orientation: complement angles near 180° are "upright"
    // effectiveTiltAngle = 180° - tiltAngle, so 8° becomes 172°, 10° becomes 170°
    isUpright = (effectiveTiltAngle > 165.0); // 165°-180° is upright in flipped orientation
  } else {
    // In normal orientation: angles near 0° are "upright"
    isUpright = (effectiveTiltAngle < 15.0); // 0°-15° is upright in normal orientation
  }
  
  // Debug: Show the actual values being used for zone detection
  Serial.print(" | isUpright:"); Serial.print(isUpright ? "YES" : "NO");
  Serial.print(" | effectiveTilt:"); Serial.print(effectiveTiltAngle, 1);
  Serial.print(" | stableCalc:"); Serial.print(lastStableWasFlipped ? "FLIP>165" : "UPRT<15");
  
  bool isSideways = (effectiveTiltAngle > 75.0 && effectiveTiltAngle < 120.0); // 75° to 120°: sideways (extended)
  
  // Debug: Show both raw and effective angles for troubleshooting
  if (lastStableWasFlipped) {
    Serial.print(" | Raw: "); Serial.print(tiltAngle, 1); Serial.print("° → Effective: "); Serial.print(effectiveTiltAngle, 1); Serial.print("°");
  }
  
    // Stable positions: both UPRIGHT (0°-15°) and FLIPPED (165°-180°) are stable
  // Check both the effective angle (for zone-based stability) and raw upside-down detection
  bool isStable;
  if (lastStableWasFlipped) {
    // In flipped orientation: effectiveTiltAngle > 165° means we're stable (upright in flipped world)
    isStable = (effectiveTiltAngle > 165.0) || isUpsideDown;
  } else {
    // In normal orientation: effectiveTiltAngle < 15° means we're stable (upright in normal world)
    isStable = (effectiveTiltAngle < 15.0) || isUpsideDown;
  }
  // Additional debug for orientation memory
  Serial.print(" | Stable:"); Serial.print(isStable ? "YES" : "NO");
  Serial.print(" | UpsideDown:"); Serial.print(isUpsideDown ? "YES" : "NO");
  

  
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

  Serial.print("\rTilt: ");
  Serial.print(tiltAngle, 1);
  Serial.print("°");
  if (lastStableWasFlipped) {
    Serial.print(" [FLIP] → effective:");
    Serial.print(effectiveTiltAngle, 1);
    Serial.print("°");
  }
  Serial.print(" | Z: ");
  Serial.print(zAccel, 2);
  Serial.print(" | Zones: ");
  Serial.print(isUpright ? "UPRIGHT" : (isSideways ? "SIDEWAYS" : "TILTED"));
  Serial.print(" | Mem:");
  Serial.print(lastStableWasFlipped ? "FLIP" : "UPRT");
  Serial.print(" | State: ");
  switch(currentState) {
    case STANDBY: Serial.print("STANDBY"); break;
    case COUNTDOWN: Serial.print("COUNTDOWN"); break;
    case PAUSED: Serial.print("PAUSED"); break;
    case COMPLETED: Serial.print("COMLPETED"); break;
  }

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
    } else if (effectiveTiltAngle > 120.0 && effectiveTiltAngle < 165.0) {
      // In CANCELED zone - stay in STANDBY until reaching UPRIGHT or FLIPPED
      Serial.println(" | CANCELED zone - Waiting for stable position");
    } else if (effectiveTiltAngle >= 15.0 && effectiveTiltAngle <= 75.0) {
      // Start countdown from TILTED zone (15°-75° effective angle, works for both orientations)
      Serial.println(" | TILT DETECTED - Starting countdown!");
      resetSystem();
      lastAnimTickMs = now;
    } else if (isStable) {
      Serial.println(" | Stable position");
    } else {
      Serial.println(" | Other position");
    }
  } else if (currentState == COUNTDOWN) {
    // Check if hourglass is in CANCELED zone - cancel countdown and wait for stable position
    if (effectiveTiltAngle > 120.0 && effectiveTiltAngle < 165.0) {
      currentState = STANDBY;
      topFilled = true;
      fallingGrain = 0;
      bottomFallPhase = 0;
      unchangedSteps = 0;
      melodyPlayed = false;
      clearGrid();
      seedSandBottom();
      Serial.println(" | COUNTDOWN CANCELLED - In CANCELED zone!");
      Serial.println("*** COUNTDOWN CANCELLED - Waiting for UPRIGHT or FLIPPED ***");
    }
    // Allow pausing from COUNTDOWN state when laid sideways
    else if (isSideways && sidewaysStable) {
      currentState = PAUSED;
      lastAnimTickMs = now;
      Serial.println(" | PAUSED");
      Serial.println("*** PAUSED STATE TRIGGERED FROM COUNTDOWN ***");
    } else {
      Serial.println(); // Just add newline for COUNTDOWN state
    }
  } else if (currentState == PAUSED) {
    if (!isSideways && sidewaysStable) {
      currentState = COUNTDOWN;
      lastAnimTickMs = now;
      Serial.println(" | RESUME COUNTDOWN");
      Serial.println("*** RESUMING FROM PAUSED STATE ***");
      Serial.println("Display active - countdown resumed");
    } else {
      Serial.println(" | PAUSED");
    }
  } else if (currentState == COMPLETED) {
    // Check if hourglass is in CANCELED zone - reset to standby and wait for stable position
    if (effectiveTiltAngle > 120.0 && effectiveTiltAngle < 165.0) {
      currentState = STANDBY;
      topFilled = true;
      fallingGrain = 0;
      bottomFallPhase = 0;
      unchangedSteps = 0;
      melodyPlayed = false;
      clearGrid();
      seedSandBottom();
      Serial.println(" | RESET - In CANCELED zone!");
      Serial.println("*** RESET - Waiting for UPRIGHT or FLIPPED ***");
    }
    else if (isSideways && sidewaysStable) {
      currentState = PAUSED;
      lastAnimTickMs = now;
      Serial.println(" | PAUSED");
      Serial.println("*** PAUSED STATE TRIGGERED FROM COMPLETED ***");
    } else if (effectiveTiltAngle >= 15.0 && effectiveTiltAngle <= 75.0) {
      // Allow countdown to restart from TILTED zone (15°-75° effective angle, works for both orientations)
      Serial.println(" | TILT DETECTED - Restarting countdown!");
      resetSystem();
      lastAnimTickMs = now;
    } else if (isStable) {
      Serial.println(" | Stable position");
    } else {
      Serial.println(" | Other position");
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
  mx.clear();

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
  }

  // Start with gravity "down" (+1). Seed initial sand.
  gravityDir = +1;
  initGrainOrder();
  seedSandBottom(); // Uncommented to initialize sand state
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
    mx.clear();
    delay(100);
  }
}
