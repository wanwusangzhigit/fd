// Progress.h — terminal progress reporter (mirrors Android UI progress).
//
// Two flavors:
//   * `ProgressLine` prints a single `\r`-overwriting line:
//       filename  [####    ] 45% · 5.3 MB / 12.0 MB · 2.5 MB/s · 3s left
//   * `SilentProgress` is a no-op for non-TTY contexts.
#pragma once

#include "Util.h"
#include <chrono>
#include <functional>
#include <string>

namespace wfd {
namespace progress {

// Construct a progress callback suitable for `tcp::SendFile` / `bt::SendFile`.
// Auto-detects TTY; falls back to no-op if not interactive.
std::function<void(int64_t, int64_t)> MakeReporter(const std::string& fileName);

} // namespace progress
} // namespace wfd
