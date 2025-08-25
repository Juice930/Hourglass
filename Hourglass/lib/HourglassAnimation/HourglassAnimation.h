#include <MD_MAX72xx.h>
extern MD_MAX72XX mx;
void stepSand();
void render();
void seedSandBottom();
void initDiamondMapping();
#ifndef HOURGLASS_ANIMATION_H
#define HOURGLASS_ANIMATION_H

#include <Arduino.h>

// Matrix and sand simulation parameters
const uint8_t H = 16;
const uint8_t W = 8;
const uint8_t N = 8;
const uint8_t GRAINS = 64;
extern bool grid[H][W];
extern bool nextGrid[H][W];
extern int gravityDir;
extern uint8_t SAND_COUNT;
extern uint8_t unchangedSteps;
extern const uint8_t UNCHANGED_STEPS_THRESHOLD;

// Animation state
extern uint8_t grainOrder[GRAINS];
extern uint8_t fallingGrain;
extern bool topFilled;
extern int fallY, fallX;
extern bool isFalling;
extern const uint16_t STEP_MS;
extern const uint16_t FALL_STEP_MS;

void initGrainOrder();
void clearGrid();
void printGridDebug();
bool isGridUnchanged();
void updateSandLogic();
void animateFallingGrain();
void updateFallingAnimation();

#endif // HOURGLASS_ANIMATION_H
