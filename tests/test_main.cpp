#include "test_main.h"

// Every test executable links this file. Kept separate from test_main.h so the
// header can be included by more than one translation unit without producing
// two definitions of main().
int main() {
    return ::testing::run_all();
}
