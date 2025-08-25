
// ================== Digital Hourglass for ESP32 ==================
// Hardware assumed: ESP32 + MPU6050 + 2x MAX7219 8x8 LED matrices (daisy-chained)
// Libraries (install via Library Manager): 
//   - MD_MAX72XX by MajicDesigns
//   - Adafruit MPU6050
//   - Adafruit Unified Sensor
//   - Wire

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
const int BUZZER_RESOLUTION = 8; // 8-bit resolution
bool melodyPlayed = false;

static void buzzerTone(unsigned int freq, unsigned int durationMs) {
  if (freq == 0) {
    ledcWrite(BUZZER_CHANNEL, 0);
    delay(durationMs);
    return;
  }
  ledcWriteTone(BUZZER_CHANNEL, freq);
  ledcWrite(BUZZER_CHANNEL, 128); // ~50% duty
  delay(durationMs);
}

static void playCompletionMelody() {
  // Simple pleasant chime sequence (not copyrighted melody)
  const unsigned int notes[] = { 988, 0, 988, 1319, 0, 1175, 988, 0, 784, 988, 0 };
  const unsigned int lens[]  = { 150, 50, 150, 250, 50, 200, 200, 50, 200, 400, 50 };
  const size_t count = sizeof(notes)/sizeof(notes[0]);
  for (size_t i = 0; i < count; i++) {
    buzzerTone(notes[i], lens[i]);
  }
  ledcWrite(BUZZER_CHANNEL, 0);
}

// -------- MPU6050 --------
Adafruit_MPU6050 mpu;

// -------- System States --------
enum SystemState {
  STANDBY,      // Waiting for tilt to start
  COUNTDOWN,    // Sand is flowing
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
#define TARGET_DURATION_MS 15000
#define TOTAL_FALL_STEPS 520
const uint16_t FALL_STEP_MS = (TARGET_DURATION_MS + (TOTAL_FALL_STEPS/2)) / TOTAL_FALL_STEPS; // rounded

// Independent bottom falling-grain animation
uint8_t bottomFallPhase = 0;       // 0..7 along the bottom diagonal

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
  
  // Print system state info (tilt info is now in checkTilt())
  Serial.print("State: ");
  switch(currentState) {
    case STANDBY: Serial.print("STANDBY"); break;
    case COUNTDOWN: Serial.print("COUNTDOWN"); break;
    case COMPLETED: Serial.print("COMPLETED"); break;
  }
  Serial.print(" | Sand: ");
  Serial.println(fallingGrain);
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
  if (topFilled) {
    // Fill the top 8x8 matrix
    for (uint8_t y = 0; y < N; y++) {
      for (uint8_t x = 0; x < N; x++) {
        grid[y][x] = true;
      }
    }
  } else {
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

// Overlay the single falling grain on the bottom diagonal
static void drawBottomFallingGrain() {
  // Show falling grain in the bottom matrix
  uint8_t y = 8 + bottomFallPhase;
  uint8_t x = bottomFallPhase;
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
  // Draw sand state
  stepSand();
  // Overlay independent falling grain
  drawBottomFallingGrain();
  // Render frame
  render();
  printGridDebug();

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
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // Calculate tilt angle from accelerometer data
  // Tilt angle = arccos(|Z| / sqrt(X² + Y² + Z²))
  float xAccel = a.acceleration.x;
  float yAccel = a.acceleration.y;
  float zAccel = a.acceleration.z;
  
  // Calculate magnitude of acceleration vector
  float magnitude = sqrt(xAccel*xAccel + yAccel*yAccel + zAccel*zAccel);
  
  // Calculate tilt angle (0° = flat, 90° = vertical)
  float tiltAngle = 0;
  if (magnitude > 0.1) { // Avoid division by zero
    tiltAngle = acos(abs(zAccel) / magnitude) * 180.0 / PI;
  }
  
  // Debug: print tilt angle
  Serial.print("Tilt: ");
  Serial.print(tiltAngle, 1);
  Serial.print("° | Z: ");
  Serial.print(zAccel, 2);
  Serial.print(" m/s²");
  
  // Consider stable if tilt is close to 0° (flat) or 180° (upside down)
  bool isStable = (tiltAngle < 15.0) || (tiltAngle > 165.0);
  
  if (currentState == STANDBY) {
    // Start countdown when tilted away from stable positions
    if (!isStable) {
      Serial.println(" | TILT DETECTED - Starting countdown!");
      resetSystem();
    } else {
      Serial.println(" | Stable position");
    }
  } else if (currentState == COMPLETED) {
    // Restart when tilted away from stable positions
    if (!isStable) {
      Serial.println(" | TILT DETECTED - Restarting countdown!");
      resetSystem();
    } else {
      Serial.println(" | Stable position");
    }
  } else {
    Serial.println(" | Countdown active");
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
  ledcSetup(BUZZER_CHANNEL, 1000, BUZZER_RESOLUTION);
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
  //seedSandBottom();
}

void loop() {
  static uint32_t lastFallStep = 0;
  static uint32_t lastTiltCheck = 0;
  uint32_t now = millis();

  // Check tilt every 100ms
  if (now - lastTiltCheck >= 100) {
    lastTiltCheck = now;
    checkTilt();
  }

  // Only run countdown when active
  if (currentState == COUNTDOWN && now - lastFallStep >= FALL_STEP_MS) {
    lastFallStep = now;
    bool touched = advanceBottomFallingGrain();
    if (touched) {
      performSandStep();
      if (!topFilled && fallingGrain >= GRAINS && !melodyPlayed) {
        melodyPlayed = true;
        currentState = COMPLETED;
        playCompletionMelody();
        Serial.println("Countdown completed! Tilt again to restart.");
      }
    }
    updateSandLogic();
  }

  // Handle standby state
  if (currentState == STANDBY) {
    // Show standby pattern or clear display
    mx.clear();
    delay(100);
  }
}
