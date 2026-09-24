// Native unit test stub for this GDExtension. Tests here build and run as a
// plain native program -- no godot-cpp linking, no running Godot process
// required -- for engine-independent logic (e.g. math/data helpers) as it
// gets added to src/. Build and run with `scons tests`.
//
// Replace this stub with real assertions once the extension has
// engine-independent logic to test.

#include <cstdio>

namespace {

int failures = 0;

void expect(bool p_condition, const char *p_label) {
	if (!p_condition) {
		std::fprintf(stderr, "FAIL: %s\n", p_label);
		failures++;
	} else {
		std::printf("PASS: %s\n", p_label);
	}
}

} // namespace

int main() {
	expect(true, "stub test placeholder");

	if (failures == 0) {
		std::printf("All tests passed.\n");
		return 0;
	}
	std::fprintf(stderr, "%d test(s) failed.\n", failures);
	return 1;
}
