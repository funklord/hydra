// The deadline every test binary carries (`watchdog.cpp`) does what it says.
//
// It was fired by hand once, on the day it was written, and then nothing
// checked it again -- a bound nobody tests is one that can stop working
// without anybody finding out until a hang runs for forty minutes again. So
// this binary runs a copy of itself that hangs, with a deadline of a second,
// and asks how it ended; and a copy with the deadline turned off, which must
// run its full time and end normally.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <cstdio>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// The copy: hang for as long as asked, which the deadline should cut short.
	if (qEnvironmentVariableIsSet("HYDRA_WATCHDOG_HANG")) {
		const int seconds = qEnvironmentVariableIntValue("HYDRA_WATCHDOG_HANG");
		for (int i = 0; i < seconds * 10; ++i)
			usleep(100000);
		return 0;
	}
	QCoreApplication app(argc, argv);

	auto run = [&](const char *deadline, int hang, int *code, QString *said, qint64 *ms) {
		QProcess p;
		QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
		env.insert("HYDRA_TEST_DEADLINE", QString::fromLatin1(deadline));
		env.insert("HYDRA_WATCHDOG_HANG", QString::number(hang));
		p.setProcessEnvironment(env);
		QElapsedTimer t;
		t.start();
		p.start(QCoreApplication::applicationFilePath(), {});
		const bool ended = p.waitForFinished(30000);
		if (!ended)
			p.kill();
		*ms = t.elapsed();
		*code = ended && p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
		*said = QString::fromUtf8(p.readAllStandardError());
	};

	section("a binary that hangs past its deadline is ended, and says why");
	{
		int code = 0;
		QString said;
		qint64 ms = 0;
		run("1", 20, &code, &said, &ms);
		check(code == 124, QString("it exits 124, as timeout would (%1)").arg(code));
		check(said.contains("HYDRA_TEST_DEADLINE passed"), "and says the deadline passed: " +
		                                                    said.trimmed());
		check(ms < 10000, QString("after the deadline, not the hang (%1 ms)").arg(ms));
	}

	section("a deadline of 0 is none");
	{
		int code = 0;
		QString said;
		qint64 ms = 0;
		run("0", 2, &code, &said, &ms);
		check(code == 0 && ms >= 1900, QString("it runs its two seconds and ends normally "
		                                       "(%1, %2 ms)").arg(code).arg(ms));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
