// Tilt logic: turns acceleration samples into two MIDI CC messages. Plain logic, no device.
#pragma once
#include <stdint.h>

namespace tilt {

// The axis mapping: which acceleration axis (0 to 2) carries each tilt, and the sign of that axis.
constexpr int FORWARD_AXIS = 1;
constexpr int RIGHT_AXIS = 0;
constexpr int FORWARD_SIGN = 1;
constexpr int RIGHT_SIGN = 1;

constexpr float DEAD_ZONE_DEGREES = 3.0f;  // a tilt inside this angle gives the middle value
constexpr float RANGE_DEGREES = 45.0f;     // a tilt of this angle or more gives 0 or 127
constexpr float SMOOTHING_SAMPLES = 5.0f;  // a value reaches its end in about this many samples

enum Cc { FORWARD_BACK = 0, LEFT_RIGHT = 1, CC_COUNT = 2 };

// One acceleration sample, in g, on the three axes of the IMU unit.
struct Sample {
  float a[3];
};

// The CC messages of one step. The filled messages sit at index 0 up to count - 1.
struct Messages {
  int count = 0;
  uint8_t m[2][3] = {};
};

class Tilt {
 public:
  Tilt();

  // Set the Zero position from one sample.
  void zero(const Sample& s);

  // Forget the last value sent for one CC. The next step with Hold off sends its current value.
  void clear(Cc cc);

  // Take one sample. The channel is 1 to 16. During Hold the step returns no message and keeps the smoothed values.
  Messages step(const Sample& s, bool hold, uint8_t forwardBackCc, uint8_t leftRightCc, uint8_t channel);

 private:
  float zeroAngle_[CC_COUNT];
  float smoothed_[CC_COUNT];
  int lastSent_[CC_COUNT];  // -1 when nothing is sent yet
};

}  // namespace tilt
