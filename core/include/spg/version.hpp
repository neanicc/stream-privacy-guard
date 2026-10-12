#pragma once

#include <string_view>

namespace spg {

// Project version from the root CMakeLists.txt, for example "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

}  // namespace spg
