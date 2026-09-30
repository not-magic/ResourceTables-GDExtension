// Native unit tests (no godot-cpp, no Godot process) for engine-independent logic; run with `scons tests`.

#include <cstdio>

namespace {

int failures = 0;

void expect(bool p_is_true, const char *p_label) {
	if (!p_is_true) {
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
