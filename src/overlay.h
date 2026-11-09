#pragma once

#include "common.h"

namespace sp {

// Runs the full-screen selection overlay modally and returns the picked
// pixels plus their screen-space origin.  std::nullopt means the user
// cancelled (Esc / right click) or the screen capture itself failed.
std::optional<SelectionResult> runSelectionOverlay(HINSTANCE instance);

}  // namespace sp
