// **A deadline inside every test binary**, linked into each by the test
// Makefile. A test that hangs runs until something outside stops it, and a
// person running the binary directly -- which is what chasing a failure
// means -- has nothing outside. Measured 2026-10-30: a loop in new code
// wrapped to its start once no dot was left, and `test_subscription` sat at
// one check for forty minutes until an outer `timeout` ended it.
//
// `HYDRA_TEST_DEADLINE` in seconds; 1200 when unset, 0 for none. On expiry it
// says so and exits 124, the code `timeout` uses, so a script reading exit
// codes cannot tell the two apart -- which is the point.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void expired(int) {
	static const char said[] = "\ntest watchdog: HYDRA_TEST_DEADLINE passed; "
	                           "a check is hung. Exiting 124.\n";
	const ssize_t ignored = write(2, said, sizeof said - 1);
	(void)ignored;
	_exit(124);
}

struct arm {
	arm() {
		unsigned seconds = 1200;
		if (const char *v = std::getenv("HYDRA_TEST_DEADLINE"))
			seconds = unsigned(std::strtoul(v, nullptr, 10));
		if (seconds == 0)
			return;
		std::signal(SIGALRM, expired);
		alarm(seconds);
	}
};

const arm armed;

}  // namespace
