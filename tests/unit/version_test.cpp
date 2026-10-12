#include <catch2/catch_test_macros.hpp>

#include "spg/version.hpp"

TEST_CASE("spg::version reports the CMake project version", "[version]") {
  CHECK(spg::version() == SPG_EXPECTED_VERSION);
  CHECK_FALSE(spg::version().empty());
}
