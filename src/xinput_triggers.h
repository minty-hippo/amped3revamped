// amped3 - analog trigger values straight from Windows' XInput.
//
// For the analog tuck: the trigger values that reach the port through the
// SDK's input layer turn out to be all-or-nothing (0 or 255) on the
// controllers tested, so the triggers are read from XInput directly as well.

#pragma once

#include <cstdint>

namespace amped3::xinput {

struct Triggers {
  int left = -1, right = -1;  // 0..255; -1: no XInput controller (or not Windows)
};

// The triggers across the XInput controllers (each the strongest of them).
// Cheap enough to call every frame: empty slots are re-checked only every
// few seconds.
Triggers Read();

// The strongest trigger, or -1 with no XInput controller.
inline int StrongestTrigger() {
  const Triggers t = Read();
  return t.left < 0 ? -1 : (t.left > t.right ? t.left : t.right);
}

}  // namespace amped3::xinput
