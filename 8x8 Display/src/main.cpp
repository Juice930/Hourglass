#include <MD_MAX72xx.h>
#include <SPI.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 2
#define CS_PIN 5

MD_MAX72XX mx(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// ---------------- Display mapping options ----------------
const uint8_t DISP_W = MAX_DEVICES * 8;
const uint8_t DISP_H = 8;

// Set this to the physical rotation of EACH 8x8 tile.
// If your text appears rotated 90° to the LEFT, pick ROT_270 to rotate it back.
enum Rotation { ROT_0, ROT_90, ROT_180, ROT_270 };
const Rotation ROT = ROT_270;     // try ROT_270 or ROT_90

const bool REVERSE_CHAIN = false; // set true if columns 0..15 are physically right->left

// Map (row,absCol) -> device-local (r,c), rotate, then back to abs column
inline void setAbsRot(uint8_t row, uint8_t absCol, bool on) {
  // optional chain flip
  if (REVERSE_CHAIN) absCol = (DISP_W - 1) - absCol;

  // split into device & local col
  uint8_t dev = absCol / 8;
  uint8_t lc  = absCol % 8;  // local col 0..7 in that 8x8
  uint8_t lr  = row;         // local row 0..7

  // rotate within the 8x8 tile
  uint8_t rr, cc;
  switch (ROT) {
    case ROT_0:   rr = lr;          cc = lc;          break;
    case ROT_90:  rr = lc;          cc = 7 - lr;      break; // 90° clockwise
    case ROT_180: rr = 7 - lr;      cc = 7 - lc;      break;
    case ROT_270: rr = 7 - lc;      cc = lr;          break; // 90° counter-clockwise
  }

  // back to absolute column
  uint8_t absCol2 = dev*8 + cc;

  // draw
  // (MD_MAX72XX absolute addressing: row, absCol)
  mx.setPoint(rr, absCol2, on);
}

inline void setAbs(uint8_t row, uint8_t absCol, bool on) {
  if (REVERSE_CHAIN) absCol = (DISP_W - 1) - absCol;  // flip columns 0..15
  mx.setPoint(row, absCol, on);
}
// ----- 5x7 FONT BITMAPS (each byte = vertical column of 7 bits) -----

// H
const uint8_t COL_H[] = {
  0b1000001,  // col0
  0b1000001,  // col1
  0b1111111,  // col2
  0b1000001,  // col3
  0b1000001,  // col4
  0b0000000   // spacer column!
};

// I
const uint8_t COL_I[] = {
  0b1111111,  // top/bottom bar
  0b0010000,  // center line
  0b0010000,
  0b0010000,
  0b1111111,
  0b0000000   // spacer column!
};

// SPACE
const uint8_t COL_SPACE[] = {
  0b0000000   // blank
};


struct Glyph { const uint8_t* cols; uint8_t width; };
Glyph glyphOf(char c) {
  switch (c) {
    case 'H': return { COL_H, sizeof(COL_H) };
    case 'I': return { COL_I, sizeof(COL_I) };
    case ' ': return { COL_SPACE, sizeof(COL_SPACE) };
    default:  return { COL_SPACE, sizeof(COL_SPACE) }; // fallback blank
  }
}

// Display buffer
bool fb[DISP_H][DISP_W];

void fbClear() {
  for (uint8_t r=0; r<DISP_H; r++)
    for (uint8_t c=0; c<DISP_W; c++)
      fb[r][c] = false;
}

void fbShiftLeftAndInsert(uint8_t colBits) {
  for (uint8_t r=0; r<DISP_H; r++) {
    for (uint8_t c=0; c<DISP_W-1; c++) {
      fb[r][c] = fb[r][c+1];
    }
  }
  for (uint8_t r=0; r<7; r++) {
    fb[r][DISP_W-1] = (colBits >> r) & 0x01;
  }
  fb[7][DISP_W-1] = false;
}

void fbRender() {
  mx.clear();
  for (uint8_t r = 0; r < DISP_H; r++) {
    for (uint8_t c = 0; c < DISP_W; c++) {
      setAbsRot(r, c, fb[r][c]);
    }
  }
}

// ---- Message stream ----
const char* MESSAGE = " HI HI  ";
uint8_t msgIndex = 0, colIndex = 0;

uint8_t nextMsgColumn() {
  Glyph g = glyphOf(MESSAGE[msgIndex]);
  uint8_t colBits = g.cols[colIndex++];
  if (colIndex >= g.width) {
    colIndex = 0;
    msgIndex++;
    if (MESSAGE[msgIndex] == '\0') msgIndex = 0;
  }
  return colBits;
}

// ---- Arduino main ----
uint32_t lastTick=0;
const uint16_t SCROLL_MS=100;

void setup() {
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 6);
  fbClear();
}

void loop() {
  if (millis()-lastTick >= SCROLL_MS) {
    lastTick = millis();
    uint8_t colBits = nextMsgColumn();
    fbShiftLeftAndInsert(colBits);
    fbRender();
  }
}
