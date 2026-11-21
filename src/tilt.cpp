// Tilt logic: turns acceleration samples into two MIDI CC messages. Plain logic, no device.
#include "tilt.h"

#include <math.h>

namespace tilt {

namespace {

constexpr float PI_VALUE = 3.14159265358979f;
constexpr float MIDDLE = 64.0f;

// The angle in degrees of the tilt on one axis. The other two axes give the reference, so the sign of gravity on them
// does not matter.
float angleOf(const Sample& s, int axis, int sign) {
  float tilted = s.a[axis] * sign;
  float other1 = s.a[(axis + 1) % 3];
  float other2 = s.a[(axis + 2) % 3];
  return atan2f(tilted, sqrtf(other1 * other1 + other2 * other2)) * 180.0f / PI_VALUE;
}

// The CC value for an angle from the Zero position: 64 in the dead zone, then a straight line to 0 or 127.
float valueOf(float angle) {
  float size = fabsf(angle);
  if (size <= DEAD_ZONE_DEGREES) return MIDDLE;
  if (size > RANGE_DEGREES) size = RANGE_DEGREES;
  float part = (size - DEAD_ZONE_DEGREES) / (RANGE_DEGREES - DEAD_ZONE_DEGREES);
  return angle > 0 ? MIDDLE + part * (127.0f - MIDDLE) : MIDDLE - part * MIDDLE;
}

void anglesOf(const Sample& s, float out[CC_COUNT]) {
  out[FORWARD_BACK] = angleOf(s, FORWARD_AXIS, FORWARD_SIGN);
  out[LEFT_RIGHT] = angleOf(s, RIGHT_AXIS, RIGHT_SIGN);
}

}  // namespace

Tilt::Tilt() {
  for (int i = 0; i < CC_COUNT; i++) {
    zeroAngle_[i] = 0;
    smoothed_[i] = MIDDLE;
    lastSent_[i] = -1;
  }
}

void Tilt::zero(const Sample& s) {
  anglesOf(s, zeroAngle_);
}

void Tilt::clear(Cc cc) {
  lastSent_[cc] = -1;
}

Messages Tilt::step(const Sample& s, bool hold, uint8_t forwardBackCc, uint8_t leftRightCc, uint8_t channel) {
  Messages out;
  if (hold) return out;
  float angles[CC_COUNT];
  anglesOf(s, angles);
  const uint8_t numbers[CC_COUNT] = {forwardBackCc, leftRightCc};
  for (int i = 0; i < CC_COUNT; i++) {
    smoothed_[i] += (valueOf(angles[i] - zeroAngle_[i]) - smoothed_[i]) / SMOOTHING_SAMPLES;
    int value = (int)lroundf(smoothed_[i]);
    if (value < 0) value = 0;
    if (value > 127) value = 127;
    if (value == lastSent_[i]) continue;
    lastSent_[i] = value;
    uint8_t* m = out.m[out.count++];
    m[0] = (uint8_t)(0xB0 + ((channel - 1) & 0x0F));
    m[1] = (uint8_t)(numbers[i] & 0x7F);
    m[2] = (uint8_t)value;
  }
  return out;
}

}  // namespace tilt
