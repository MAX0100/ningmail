#include <catch2/catch_test_macros.hpp>

#include "Version.h"

TEST_CASE("version is exposed", "[version]") {
    REQUIRE(ningmail::version() == "0.1.0");
}
