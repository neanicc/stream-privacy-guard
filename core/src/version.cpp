#include "spg/version.hpp"

namespace spg {

std::string_view version() noexcept {
  return SPG_VERSION_STRING;
}

}  // namespace spg
