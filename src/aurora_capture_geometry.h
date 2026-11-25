#pragma once

#include <cstddef>
#include <optional>
#include <utility>

namespace platf {
  using aurora_capture_dimensions = std::pair<std::size_t, std::size_t>;

  // The current source geometry for a new stream, if the capture target can be deduced.
  std::optional<aurora_capture_dimensions> aurora_capture_size();
}
