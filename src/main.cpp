// cardbuddy: fm1buddy's core (fm1core) on the M5 Cardputer, with a keyboard UI
#include <M5Cardputer.h>

#include "board.h"
#include "hal.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // (with the keyboard)
  M5Cardputer.Display.setRotation(1);
  board::begin();
  ui::begin();
  hal::logRam("up");
}

void loop() {
  M5Cardputer.update();
  ui::loop();
  delay(5);
}
