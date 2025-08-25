// Simple test file to verify function declarations work
typedef unsigned char uint8_t;

// Mock MD_MAX72XX class for testing
class MockMD_MAX72XX {
public:
    void setPoint(int row, int col, bool state) {}
    void begin() {}
    void control(int param, int value) {}
    void clear() {}
};

MockMD_MAX72XX mx;

// Function declarations
void drawH(uint8_t dev);
void drawI(uint8_t dev);

// Function definitions
void drawH(uint8_t dev) {
    for (uint8_t row = 0; row < 8; row++) {
        mx.setPoint(row, dev * 8 + 0, true);
        mx.setPoint(row, dev * 8 + 7, true);
    }
}

void drawI(uint8_t dev) {
    for (uint8_t col = 0; col < 8; col++) {
        mx.setPoint(0, dev * 8 + col, true);
        mx.setPoint(7, dev * 8 + col, true);
    }
}

void setup() {
    mx.begin();
    drawH(0);  // This should work now
    drawI(1);  // This should work now
}

int main() {
    setup();
    return 0;
}
