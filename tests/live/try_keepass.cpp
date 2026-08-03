// The KeePassXC bridge, against a real KeePassXC (architecture doc §13.1).
//
// Wired since step 8 and never once run, which in this project is the same
// sentence as "probably broken" — the whole of §13.1 is transport, framing and
// end-to-end crypto, and none of it had ever met the other end.
//
// **What this needs**, and it will say so rather than passing vacuously: a
// running KeePassXC with browser integration enabled. A self-contained one is
// enough and is what the notes describe setting up — its own config file, its own
// database, nothing touching the user's.
//
// **What it cannot do alone.** `associate()` makes KeePassXC show a dialog that a
// human confirms, by design: the pairing is the moment the user decides this
// program may read their vault, and a browser that could grant itself that would
// be the bug. So the association step runs only under
// `HYDRA_KEEPASS_INTERACTIVE=1`, and the run says plainly which parts went
// unchecked. Everything before it — socket, handshake, key exchange, the
// not-associated path — runs unattended.
#include "keepass_bridge.h"
#include "keepass_protocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalSocket>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }

// Wait for one of several outcomes rather than a fixed sleep: a handshake that
// is going to fail usually fails at once, and one that works should not cost a
// second of wall clock to prove.
static bool wait_until(const std::function<bool()> &done, int max_ms) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < max_ms) {
		QEventLoop l;
		QTimer::singleShot(25, &l, &QEventLoop::quit);
		l.exec();
	}
	return done();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("what is needed to run at all");
	check(keepass_bridge::supported(),
	      "built with libsodium — the protocol is encrypted end to end");
	if (!keepass_bridge::supported()) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}

	const QString sock = keepass_bridge::socket_path();
	std::printf("  --    socket: %s\n", qPrintable(sock));

	// Connect, do not merely look.
	//
	// The first version of this checked QFile::exists on the path and reported
	// "KeePassXC is listening" — and that path is a symlink into the runtime
	// directory which **outlives the process**. So after KeePassXC exited, this
	// driver announced a listening server, then failed the handshake, and the
	// one check that was supposed to establish the precondition was the one
	// lying about it. A test that reports a passing precondition it did not test
	// is worse than no precondition check.
	{
		QLocalSocket probe;
		probe.connectToServer(sock);
		const bool up = probe.waitForConnected(1500);
		probe.abort();
		if (!up) {
			note(QFile::exists(sock)
			         ? "the socket path exists but nothing answers — a stale "
			           "socket left by a KeePassXC that has exited."
			         : "no socket at all — start KeePassXC with browser "
			           "integration enabled.");
			note("Nothing below could mean anything, so this stops rather than");
			note("reporting passes for a bridge that talked to nobody.");
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return 1;
		}
		check(true, "something is listening on the socket, not just a path");
	}

	section("handshake: change-public-keys");
	keepass_bridge bridge;
	bool ready = false, failed = false;
	QString last_error;
	QObject::connect(&bridge, &keepass_bridge::ready, [&] { ready = true; });
	QObject::connect(&bridge, &keepass_bridge::error, [&](const QString &m) {
		failed = true;
		last_error = m;
	});

	bridge.start();
	wait_until([&] { return ready || failed; }, 4000);
	check(ready, QString("the key exchange completes (%1)")
	                 .arg(ready ? QStringLiteral("ready") : last_error));
	check(bridge.connected(), "and the socket stays up afterwards");
	if (!ready) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}

	section("before pairing, it says so rather than guessing");
	check(!bridge.associated(), "a fresh bridge is not associated");
	{
		// A saved pairing that KeePassXC has never heard of. This is the case on
		// every first run after the settings file is copied to a new machine, and
		// the honest answer is "no", not an error and not a silent yes.
		bool answered = false, ok = true;
		QString message;
		QObject::connect(&bridge, &keepass_bridge::associated_changed,
		                 [&](bool good, const QString &m) {
			answered = true;
			ok = good;
			message = m;
		});
		bridge.set_association("hydra-not-a-real-pairing",
		                        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
		bridge.test_association();
		wait_until([&] { return answered; }, 4000);
		check(answered, "a bogus pairing gets an answer rather than silence");
		check(answered && !ok,
		      QString("and the answer is no (%1)").arg(message));
	}

	section("pairing");
	if (qEnvironmentVariableIntValue("HYDRA_KEEPASS_INTERACTIVE") != 1) {
		note("skipped: association needs a human to confirm the dialog in");
		note("KeePassXC, which is the point of it. Re-run with");
		note("HYDRA_KEEPASS_INTERACTIVE=1 and accept the prompt to check the");
		note("rest — associate, then request_logins against the vault.");
	} else {
		bool answered = false, ok = false;
		QString message;
		QObject::connect(&bridge, &keepass_bridge::associated_changed,
		                 [&](bool good, const QString &m) {
			answered = true;
			ok = good;
			message = m;
		});
		std::printf("  --    confirm the association dialog in KeePassXC now...\n");
		bridge.associate();
		wait_until([&] { return answered; }, 60000);
		check(answered && ok, QString("association succeeds once confirmed (%1)")
		                          .arg(message));
		check(!bridge.association_id().isEmpty(),
		      "and it hands back an id to save");
		check(!bridge.association_key().isEmpty(), "and a key with it");

		if (ok) {
			QList<credential> got;
			bool arrived = false;
			QObject::connect(&bridge, &keepass_bridge::logins,
			                 [&](int tag, const QList<credential> &entries) {
				if (tag != 7)
					return;
				got = entries;
				arrived = true;
			});
			bridge.request_logins("http://127.0.0.1:9931", 7);
			wait_until([&] { return arrived; }, 20000);
			check(arrived, "a login request for a url in the vault is answered");
			check(arrived && !got.isEmpty(),
			      QString("and the entry comes back (%1 found)").arg(got.size()));
			if (!got.isEmpty()) {
				check(got.first().login == "alice",
				      QString("with the username from the vault (%1)")
				          .arg(got.first().login));
				// Printed as a length, not a value: a password in a log is a
				// password on disk, and this one is only a fixture today.
				check(!got.first().password.isEmpty(),
				      QString("and a password, %1 characters, not shown here")
				          .arg(got.first().password.size()));
			}

			QList<credential> none;
			bool answered2 = false;
			QObject::connect(&bridge, &keepass_bridge::logins,
			                 [&](int tag, const QList<credential> &entries) {
				if (tag != 8)
					return;
				none = entries;
				answered2 = true;
			});
			bridge.request_logins("http://no-such-site.invalid", 8);
			wait_until([&] { return answered2; }, 20000);
			check(answered2 && none.isEmpty(),
			      "a url with nothing stored comes back empty rather than wrong");
		}
	}

	bridge.disconnect_now();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
