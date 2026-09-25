// Shared main() for the vortex_tests binary.
#include "vortex_test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    return vortex::testing::run_all("vortex_tests", filter);
}
