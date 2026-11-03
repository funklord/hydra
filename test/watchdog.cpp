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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

namespace {

void expired(int) {
	static const char said[] = "\ntest watchdog: HYDRA_TEST_DEADLINE passed; "
	                           "a check is hung. Exiting 124.\n";
	const ssize_t ignored = write(2, said, sizeof said - 1);
	(void)ignored;
	_exit(124);
}

// **A backtrace when a binary crashes, where no debugger is to hand**:
// `HYDRA_TEST_BACKTRACE=1`. Found needed 2026-11-03, when `try_autofill`
// segfaulted at exit in the hull rig after every check had passed and the
// machine had no gdb. Each frame is printed as file+offset, which
// `addr2line -f -C -e <file> <offset>` resolves on the host against the same
// binary. Opt-in, because the sanitizers install a SEGV handler of their own
// and this one would take its place. Not async-signal-safe in every call;
// the process is ending either way, and the signal is re-raised so the exit
// status is what it would have been.
void crashed(int sig) {
	void *frames[64];
	const int n = backtrace(frames, 64);
	char line[512];
	int len = std::snprintf(line, sizeof line, "\ntest crash: signal %d, %d frame(s)\n", sig, n);
	ssize_t ignored = write(2, line, size_t(len));
	for (int i = 0; i < n; ++i) {
		Dl_info info{};
		if (dladdr(frames[i], &info) && info.dli_fname) {
			len = std::snprintf(line, sizeof line, "  #%-2d %s+0x%lx %s\n", i,
			                    info.dli_fname,
			                    (unsigned long)((char *)frames[i] - (char *)info.dli_fbase),
			                    info.dli_sname ? info.dli_sname : "");
		} else {
			len = std::snprintf(line, sizeof line, "  #%-2d %p\n", i, frames[i]);
		}
		ignored = write(2, line, size_t(len));
	}
	(void)ignored;
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

struct arm {
	arm() {
		if (const char *v = std::getenv("HYDRA_TEST_BACKTRACE"); v && *v == '1') {
			std::signal(SIGSEGV, crashed);
			std::signal(SIGABRT, crashed);
			std::signal(SIGBUS, crashed);
		}
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
