
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

// -------- MPU6050 --------
// Adafruit_MPU6050 mpu; // Commented out for now

// -------- Simulation grid --------
// Falling animation state
int fallY = -1, fallX = -1; // Current position of falling grain
bool isFalling = false; // If true, animating a grain falling
const uint16_t FALL_STEP_MS = 100; // Speed of falling animation
// 16 rows (two 8x8), 8 columns
const uint8_t H = 16;
const uint8_t W = 8;
bool grid[H][W];
bool nextGrid[H][W];
// Track unchanged steps for auto-flip
uint8_t unchangedSteps = 0;
const uint8_t UNCHANGED_STEPS_THRESHOLD = 10; // Number of steps before auto-flip

uint32_t lastStep = 0;
const uint16_t STEP_MS = 500;     // animation speed

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

// ---------------------- Helpers ----------------------
void clearGrid() {
  for (uint8_t y=0; y<H; y++)
    for (uint8_t x=0; x<W; x++)
      grid[y][x] = false;
}

const uint8_t N = 8; // 8x8 matrix
const uint8_t GRAINS = 64;
uint8_t grainOrder[GRAINS]; // Diagonal order
uint8_t fallingGrain = 0; // Current grain to animate
bool topFilled = true; // If true, top is filled, grains are falling

// Encapsulated debug print
void printGridDebug() {
  uint16_t sandInBottom = 0;
  uint8_t yStart = (gravityDir == +1) ? 8 : 0;
  uint8_t yEnd   = (gravityDir == +1) ? 16 : 8;
  for (uint8_t y = yStart; y < yEnd; y++) {
    for (uint8_t x = 0; x < W; x++) {
      if (grid[y][x]) sandInBottom++;
    }
  }
  Serial.print("Sand in bottom/top: ");
  Serial.println(sandInBottom);
  Serial.println("Grid:");
  for (uint8_t y = 0; y < H; y++) {
    for (uint8_t x = 0; x < W; x++) {
      Serial.print(grid[y][x] ? "#" : ".");
    }
    Serial.println();
  }
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

// Encapsulated falling animation
void animateFallingGrain() {
  // Fill top except for grains that have fallen
  for (uint8_t i = fallingGrain + (isFalling ? 1 : 0); i < GRAINS; i++) {
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
  // Animate the falling grain
  if (fallingGrain < GRAINS) {
    if (!isFalling) {
      uint8_t idx = grainOrder[fallingGrain];
      fallY = idx / N;
      fallX = idx % N;
      isFalling = true;
    }
    // Move grain one step
    if (fallY < N-1) {
      // Try to fall diagonally left or right if possible
      bool leftBlocked = (fallX > 0) ? grid[fallY+1][fallX-1] : true;
      bool rightBlocked = (fallX < N-1) ? grid[fallY+1][fallX+1] : true;
      if (!leftBlocked) {
        fallY++;
        fallX--;
      } else if (!rightBlocked) {
        fallY++;
        fallX++;
      } else if (!grid[fallY+1][fallX]) {
        fallY++;
      }
    }
    // Show grain at its current position
    if (fallY < N) {
      grid[fallY][fallX] = true;
    } else {
      // In bottom matrix, mirror X
      grid[(fallY-N)+N][N-1-fallX] = true;
    }
    // If landed (next position is blocked or at bottom), finish falling
    bool landed = false;
    if (fallY == N-1) landed = true;
    else if ((fallY < N-1) && (grid[fallY+1][fallX] || (fallX > 0 && grid[fallY+1][fallX-1]) || (fallX < N-1 && grid[fallY+1][fallX+1]))) landed = true;
    if (landed) {
      isFalling = false;
      fallingGrain++;
      fallY = -1; fallX = -1;
    }
  } else {
    // Reset after a pause
    static uint8_t pause2 = 0;
    pause2++;
    if (pause2 > 20) {
      fallingGrain = 0;
      topFilled = true;
      pause2 = 0;
    }
    isFalling = false;
    fallY = -1; fallX = -1;
  }
}

void stepSand() {
  clearGrid();
  if (topFilled) {
    // Fill the top 8x8 matrix
    for (uint8_t y = 0; y < N; y++) {
      for (uint8_t x = 0; x < N; x++) {
        grid[y][x] = true;
      }
    }
    // Start falling animation after a short pause
    static uint8_t pause = 0;
    pause++;
    if (pause > 10) {
      topFilled = false;
      pause = 0;
    }
    isFalling = false;
    fallY = -1; fallX = -1;
  } else {
    animateFallingGrain();
  }
}

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

  // ...existing code...
}

// Encapsulated falling animation update
void updateFallingAnimation() {
  clearGrid();
  animateFallingGrain();
  render();
}

void updateSandLogic() {
  stepSand();
  render();
  printGridDebug();
  if (isGridUnchanged()) {
    unchangedSteps++;
  } else {
    unchangedSteps = 0;
  }
  // Only flip if grid is unchanged for several steps and all sand is in bottom/top
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


// void maybeFlipFromIMU() {
//   sensors_event_t a, g, temp;
//   mpu.getEvent(&a, &g, &temp);
//
//   // Use gravity vector sign on Z (or Y) to decide orientation.
//   // When z changes sign decisively, we flip.
//   // Threshold ~ +/-6 m/s^2 to avoid tiny shakes.
//   int newDir = (a.acceleration.z < -6.0) ? +1 : (a.acceleration.z > 6.0 ? -1 : gravityDir);
//
//   if (newDir != gravityDir && (millis() - lastFlipMs) > FLIP_DEBOUNCE_MS) {
//     gravityDir = newDir;
//     lastFlipMs = millis();
//     seedSandBottom();  // reset sand to the "new bottom"
//   }
// }

// ---------------------- Setup & Loop ----------------------
void setup() {
  Wire.begin(); // SDA=21, SCL=22 on ESP32 by default
  Serial.begin(115200);

  // MAX7219
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 6); // 0..15
  mx.clear();

  // MPU6050
  // if (!mpu.begin()) {
  //   // If no sensor, still run animation (gravity won't flip)
  //   Serial.println("MPU6050 not found! Running without flip.");
  // } else {
  //   mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  //   mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  //   mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  // }

  // Start with gravity "down" (+1). Seed initial sand.
  gravityDir = +1;
  initGrainOrder();
  //seedSandBottom();
}

void loop() {
  // maybeFlipFromIMU(); // Commented out for now
  static uint32_t lastSandStep = 0;
  static uint32_t lastFallStep = 0;
  uint32_t now = millis();

  // Only update sand state when not falling
  if (!isFalling && now - lastSandStep >= STEP_MS) {
    lastSandStep = now;
    updateSandLogic();
  }

  // Update falling animation at a faster rate
  if (isFalling && now - lastFallStep >= FALL_STEP_MS) {
    lastFallStep = now;
    updateFallingAnimation();
  }
}
