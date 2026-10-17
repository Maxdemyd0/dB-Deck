#include <cstring>

#include "TestFramework.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0, skipped = 0;
    for (auto& t : dbtest::registry()) {
        if (filter && !std::strstr(t.name, filter)) {
            ++skipped;
            continue;
        }
        try {
            t.fn();
            ++passed;
            std::cout << "[PASS] " << t.name << "\n";
        } catch (const dbtest::Failure& f) {
            ++failed;
            std::cout << "[FAIL] " << t.name << "\n       " << f.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[FAIL] " << t.name << "\n       exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed";
    if (skipped) std::cout << ", " << skipped << " filtered";
    std::cout << "\n";
    return failed ? 1 : 0;
}
