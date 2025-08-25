// External matrix object
#include <MD_MAX72xx.h>
extern MD_MAX72XX mx;

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
    for (uint8_t y=0; y<H; y++) {
        uint8_t dev = (y < 8) ? 0 : 1;
        uint8_t row = (y < 8) ? y : (y - 8);
        for (uint8_t x=0; x<W; x++) {
            bool on = grid[y][x];
            uint8_t absCol = dev * 8 + x;
            mx.setPoint(row, absCol, on);
        }
    }
}

void seedSandBottom() {
    uint16_t placed = 0;
    for (int y = 0; y < H && placed < SAND_COUNT; y++) {
        for (int x = 0; x < W && placed < SAND_COUNT; x++) {
            if (abs((x - (W-1)/2.0) - (y - (H-1)/2.0)) <= (W-1)/2.0 && abs((x - (W-1)/2.0) + (y - (H-1)/2.0)) <= (W-1)/2.0) {
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
    // ...existing code for diamond mapping...
}
#include "HourglassAnimation.h"
#include <Arduino.h>

// Matrix and sand simulation parameters
bool grid[H][W];
bool nextGrid[H][W];
int gravityDir = 1;
uint8_t SAND_COUNT = GRAINS;
uint8_t unchangedSteps = 0;
const uint8_t UNCHANGED_STEPS_THRESHOLD = 10;

// Animation state
uint8_t grainOrder[GRAINS];
uint8_t fallingGrain = 0;
bool topFilled = false;
int fallY = -1, fallX = -1;
bool isFalling = false;
const uint16_t STEP_MS = 120;
const uint16_t FALL_STEP_MS = 30;

void initGrainOrder() {
    // ...existing code for initializing grain order...
}

void clearGrid() {
    // ...existing code for clearing grid...
}

void printGridDebug() {
    // ...existing code for printing grid debug...
}

bool isGridUnchanged() {
    // ...existing code for checking unchanged grid...
    return false;
}

void updateSandLogic() {
    // ...existing code for sand logic update...
}

void animateFallingGrain() {
    // ...existing code for falling grain animation...
}

void updateFallingAnimation() {
    // ...existing code for updating falling animation...
}
