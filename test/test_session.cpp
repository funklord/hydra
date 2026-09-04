// Reading another browser's open tabs off disk (sec 4).
//
// The load-bearing check here is the decompressor. Firefox's session file is a
// raw LZ4 block behind a `mozLz40\0` header, and this project implements that
// itself rather than linking liblz4 -- which is only defensible if the output
// is compared against an implementation nobody here wrote. So the real 1.5 MB
// file from this machine's Firefox profile is decompressed both ways and the
// bytes are compared, and the suite says plainly when it could not do that.
#include "session_import.h"
#include "session_mirror.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	section("the container");
	{
		QString err;
		check(session_import::mozlz4_decompress("not a session file", &err).isEmpty(),
		      "a file that is not mozlz4 is refused");
		check(err.contains("magic"), QString("saying why (%1)").arg(err));

		// A correct header whose payload is nonsense. The point is that it comes
		// back as an error rather than as a walk off the end of a buffer.
		QByteArray bad("mozLz40\0", 8);
		bad.append(QByteArray::fromHex("10270000"));   // claims 10000 bytes
		bad.append(QByteArray::fromHex("ffffffff"));
		err.clear();
		check(session_import::mozlz4_decompress(bad, &err).isEmpty(),
		      "a truncated block is refused rather than over-read");
		check(!err.isEmpty(), QString("with a reason (%1)").arg(err));

		err.clear();
		check(session_import::lz4_block_decompress(QByteArray(), 1 << 30, &err)
		          .isEmpty(),
		      "and an implausible size is refused before anything is allocated");
	}

	section("the decompressor, against one nobody here wrote");
	{
		// Located rather than assumed: this only runs where a Firefox profile
		// with a session actually exists.
		const QString profile = session_import::firefox_profile();
		const QString path    = session_import::firefox_session_path(profile);
		if (path.isEmpty()) {
			note("skipped: no Firefox session file on this machine.");
			note("The decompressor is then unverified against a reference, which");
			note("is the one thing that makes a hand-written one trustworthy.");
		} else {
			QFile f(path);
			check(f.open(QIODevice::ReadOnly), QString("the session file opens (%1)").arg(path));
			const QByteArray raw = f.readAll();
			f.close();

			QString err;
			const QByteArray mine = session_import::mozlz4_decompress(raw, &err);
			check(!mine.isEmpty(),
			      QString("and decompresses here (%1 bytes%2)")
			          .arg(mine.size()).arg(err.isEmpty() ? "" : ", " + err));

			// The reference: python's lz4 binding, which is not this code.
			QTemporaryDir tmp;
			const QString out = tmp.path() + "/ref.json";
			QProcess py;
			py.start("python3", { "-c",
				"import sys,lz4.block\n"
				"d=open(sys.argv[1],'rb').read()\n"
				"open(sys.argv[2],'wb').write(lz4.block.decompress(d[8:]))\n",
				path, out });
			py.waitForFinished(30000);
			if (py.exitStatus() != QProcess::NormalExit || py.exitCode() != 0) {
				note("skipped the comparison: python3 lz4.block is unavailable here.");
				note("stderr: " + QString::fromUtf8(py.readAllStandardError()).trimmed());
			} else {
				QFile rf(out);
				rf.open(QIODevice::ReadOnly);
				const QByteArray reference = rf.readAll();
				check(!reference.isEmpty(), "the reference produced output");
				// The whole argument for a hand-written decompressor.
				check(mine == reference,
				      QString("the build's decoder is byte for byte identical "
				               "over %1 bytes (%2)")
				          .arg(reference.size())
				          .arg(session_import::using_system_lz4() ? "liblz4"
				                                                   : "built-in"));

				// **And the other one, whichever it is.** With liblz4 present
				// the built-in is not on any code path the app takes, which is
				// exactly how a fallback rots: it keeps compiling, nothing runs
				// it, and it is broken by the time the machine without liblz4
				// needs it. So it is driven here regardless.
				const quint8 *h =
				  reinterpret_cast<const quint8 *>(raw.constData()) + 8;
				const int declared = int(quint32(h[0]) | (quint32(h[1]) << 8) |
				                          (quint32(h[2]) << 16) |
				                          (quint32(h[3]) << 24));
				QString berr;
				const QByteArray built =
				  session_import::lz4_block_builtin(raw.mid(12), declared, &berr);
				check(built == reference,
				      QString("and the built-in decoder agrees with it too%1")
				          .arg(berr.isEmpty() ? "" : " -- " + berr));
			}

			section("the session document");
			QString perr;
			const auto tabs = session_import::parse_firefox_session(mine, &perr);
			check(!tabs.isEmpty(),
			      QString("tabs are found (%1%2)").arg(tabs.size())
			          .arg(perr.isEmpty() ? "" : ", " + perr));
			if (!tabs.isEmpty()) {
				bool all_have_urls = true;
				for (const auto &t : tabs)
					if (t.url.isEmpty() || t.title.isEmpty())
						all_have_urls = false;
				check(all_have_urls,
				      "every one has an address and a label, so no row is blank");
				note(QString("first: %1  <%2>")
				         .arg(tabs.first().title.left(48), tabs.first().url.left(60)));
			}
		}
	}

	section("a tab's past, on a document written here");
	{
		// The real profile above is only read if one exists, and it would not
		// reliably contain either case that matters. Both are built here.
		//
		// Tab one pressed Back once: `index` is 1-based and points into the
		// middle, so the imported address is the entry it is on and the two
		// either side of it survive as history.
		//
		// Tab two went A, B, A and is on the *second* A, with an entry
		// carrying no url in the middle of it. That is the pair of traps: the
		// position cannot be found by matching the url, which would answer 0,
		// and it cannot be the index into `entries` either, because the blank
		// one is dropped from the history list and shifts everything after it.
		const QByteArray doc = R"({"windows":[{"tabs":[
		  {"index":2,"entries":[
		    {"url":"https://a.test/1","title":"One"},
		    {"url":"https://a.test/2","title":"Two"},
		    {"url":"https://a.test/3","title":"Three"}]},
		  {"index":4,"entries":[
		    {"url":"https://b.test/a","title":"A"},
		    {"title":"an entry with no address at all"},
		    {"url":"https://b.test/b","title":"B"},
		    {"url":"https://b.test/a","title":"A again"}]}]}]})";

		QString herr;
		const auto tabs = session_import::parse_firefox_session(doc, &herr);
		check(tabs.size() == 2,
		      QString("both tabs parse (%1%2)").arg(tabs.size())
		          .arg(herr.isEmpty() ? "" : ", " + herr));
		if (tabs.size() == 2) {
			const auto &one = tabs.at(0);
			check(one.url == "https://a.test/2",
			      QString("a tab that went Back imports the page it is on (%1)")
			          .arg(one.url));
			check(one.history.entries.size() == 3,
			      QString("with the whole list, not just what it is on (%1)")
			          .arg(one.history.entries.size()));
			check(one.history.entries.size() == 3 &&
			          one.history.entries.at(0).url == "https://a.test/1" &&
			          one.history.entries.at(2).url == "https://a.test/3",
			      "oldest first, so Forward is not lost by importing");
			check(one.history.index == 1,
			      QString("and it knows where in that list it stands (%1)")
			          .arg(one.history.index));

			const auto &two = tabs.at(1);
			check(two.history.entries.size() == 3,
			      QString("an entry with no address is dropped (%1)")
			          .arg(two.history.entries.size()));
			check(two.history.index == 2,
			      QString("and the position survives both the duplicate url "
			              "and the dropped entry (%1, not 0)")
			          .arg(two.history.index));
			check(two.history.index >= 0 &&
			          two.history.index < two.history.entries.size() &&
			          two.history.entries.at(two.history.index).url == two.url,
			      "the entry it points at is the address the tab imported as");
		}
	}

	section("which profile, which is where an importer quietly imports nothing");
	{
		// The trap, reproduced: a profiles.ini whose Default=1 names a stub and
		// whose install-locked entry names the real one.
		QTemporaryDir tmp;
		QFile ini(tmp.path() + "/profiles.ini");
		ini.open(QIODevice::WriteOnly);
		ini.write("[Profile1]\nName=default\nPath=stub.default\nDefault=1\n\n"
		           "[Profile0]\nName=default-esr\nPath=real.default-esr\n\n"
		           "[Install123]\nDefault=real.default-esr\nLocked=1\n");
		ini.close();
		const QString picked = session_import::firefox_profile(tmp.path());
		check(picked.endsWith("real.default-esr"),
		      QString("the install-locked profile wins over Default=1 (%1)")
		          .arg(QFileInfo(picked).fileName()));

		// And with no Install section, Default=1 is the right answer.
		QTemporaryDir tmp2;
		QFile ini2(tmp2.path() + "/profiles.ini");
		ini2.open(QIODevice::WriteOnly);
		ini2.write("[Profile0]\nName=only\nPath=only.default\nDefault=1\n");
		ini2.close();
		check(session_import::firefox_profile(tmp2.path()).endsWith("only.default"),
		      "and is used when nothing overrides it");

		check(session_import::firefox_profile(tmp.path() + "/nope").isEmpty(),
		      "a root with no profiles.ini yields nothing rather than a guess");
	}

	section("a file change is not a tab change");
	{
		// The property that decides whether polling is usable at all. Firefox
		// rewrites its session file constantly -- scroll offsets, form state,
		// which tab has focus -- so a poller that refreshed on every write
		// would rebuild the mirror every few seconds while the set of tabs sat
		// perfectly still, folding the tree about under the user's hands.
		auto tab = [](const char *title, const char *url, int window) {
			session_import::imported_tab t;
			t.title = title; t.url = url; t.window = window;
			return t;
		};
		QList<session_import::imported_tab> a{
			tab("Docs", "https://a.test/1", 0),
			tab("Mail", "https://b.test/2", 0) };

		QList<session_import::imported_tab> same = a;
		check(session_mirror::fingerprint(a) == session_mirror::fingerprint(same),
		      "the same tabs fingerprint the same");

		// A tab dragged between two Firefox windows is the same tab; rebuilding
		// the mirror for it would be churn with nothing to show.
		QList<session_import::imported_tab> moved{
			tab("Docs", "https://a.test/1", 1),
			tab("Mail", "https://b.test/2", 1) };
		check(session_mirror::fingerprint(a) == session_mirror::fingerprint(moved),
		      "and so do the same tabs in a different window");

		QList<session_import::imported_tab> renamed{
			tab("Docs — edited", "https://a.test/1", 0),
			tab("Mail", "https://b.test/2", 0) };
		check(session_mirror::fingerprint(a) != session_mirror::fingerprint(renamed),
		      "a retitled tab is a change, since that is what the row shows");

		// The case a count would miss entirely.
		QList<session_import::imported_tab> swapped{
			tab("Docs", "https://a.test/1", 0),
			tab("News", "https://c.test/3", 0) };
		check(session_mirror::fingerprint(a) != session_mirror::fingerprint(swapped),
		      "and closing one tab while opening another is too, which a count "
		      "of tabs would have called identical");

		check(session_mirror::fingerprint({}) !=
		          session_mirror::fingerprint(a),
		      "no tabs is distinguishable from some tabs");
	}

	section("the poller against a file that really changes");
	{
		// Driven rather than reasoned about: a real file, rewritten underneath
		// a real poller, with the emissions counted.
		QTemporaryDir tmp;
		const QString path = tmp.path() + "/recovery.jsonlz4";

		// Build genuine mozlz4 files, so this exercises the same read path the
		// app uses rather than a stub of it.
		auto write_session = [&](const QString &second_url) {
			const QByteArray json = QString(
			  "{\"windows\":[{\"tabs\":["
			  "{\"index\":1,\"entries\":[{\"url\":\"https://a.test/1\",\"title\":\"One\"}]},"
			  "{\"index\":1,\"entries\":[{\"url\":\"%1\",\"title\":\"Two\"}]}"
			  "]}]}").arg(second_url).toUtf8();
			// Store the payload uncompressed-but-valid: a run of literals is a
			// legal LZ4 block, which keeps this test independent of any
			// compressor.
			QByteArray block;
			int off = 0;
			while (off < json.size()) {
				const int run = qMin(json.size() - off, 200);
				if (run < 15) {
					block.append(char(run << 4));
				} else {
					block.append(char(15 << 4));
					int rest = run - 15;
					while (rest >= 255) { block.append(char(255)); rest -= 255; }
					block.append(char(rest));
				}
				block.append(json.mid(off, run));
				off += run;
			}
			QByteArray file("mozLz40\0", 8);
			const quint32 n = quint32(json.size());
			file.append(char(n & 0xff)); file.append(char((n >> 8) & 0xff));
			file.append(char((n >> 16) & 0xff)); file.append(char((n >> 24) & 0xff));
			file.append(block);
			QFile f(path);
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write(file);
			f.close();
		};

		write_session("https://b.test/2");
		// The fixture has to be readable by the real reader, or everything
		// below measures the fixture.
		QString ferr;
		const auto sanity = session_import::firefox_tabs(path, &ferr);
		check(sanity.size() == 2,
		      QString("the fixture is a real mozlz4 file the reader accepts (%1%2)")
		          .arg(sanity.size()).arg(ferr.isEmpty() ? "" : ", " + ferr));

		session_mirror mirror;
		int changes = 0;
		QObject::connect(&mirror, &session_mirror::tabs_changed,
		                 [&](const QList<session_import::imported_tab> &) { ++changes; });
		mirror.start("firefox", path, 60000);   // long interval; polls are driven by hand
		check(changes == 1, "starting it reports once, so turning it on does something");

		check(!mirror.poll_once(),
		      "polling an untouched file reports nothing");
		check(changes == 1, "and emits nothing");

		// Rewritten with the same tabs, which is what Firefox does all day.
		QThread::msleep(1100);   // mtime granularity is a second on some systems
		write_session("https://b.test/2");
		check(!mirror.poll_once(),
		      "a rewritten file holding the same tabs reports nothing");
		check(changes == 1,
		      "and still emits nothing -- this is the whole reason for the "
		      "fingerprint");

		QThread::msleep(1100);
		write_session("https://c.test/3");
		check(mirror.poll_once(), "a genuinely different tab set does report");
		check(changes == 2, "and emits exactly once for it");
	}

	section("Chromium, which writes a command log rather than a document");
	{
		// Every constant here came from Chromium's own source -- vendored in
		// this tree because Qt WebEngine bundles it -- and was then checked
		// against a live file. Unlike the Firefox path there is **no reference
		// implementation to compare against**: nothing else on a normal machine
		// reads these. So this checks structure and plausibility, which is
		// weaker, and says so rather than implying otherwise.
		QString err;

		QByteArray junk("not a session file at all");
		check(session_import::replay_snss(junk, &err).isEmpty(),
		      "a file that is not SNSS is refused");
		check(err.contains("signature"), QString("saying why (%1)").arg(err));

		// The version is internal API with no stability promise, so an unknown
		// one is refused *by number* rather than parsed hopefully.
		QByteArray future;
		future.append(QByteArray::fromHex("534e5353"));   // 'SNSS'
		future.append(QByteArray::fromHex("63000000"));   // version 99
		err.clear();
		check(session_import::replay_snss(future, &err).isEmpty(),
		      "and so is a version this reader does not know");
		check(err.contains("99"),
		      QString("naming the version, which is the first thing worth "
		               "knowing when this breaks (%1)").arg(err));

		// Encrypted files are versions 2 and 4, and there is no key here.
		QByteArray encrypted;
		encrypted.append(QByteArray::fromHex("534e5353"));
		encrypted.append(QByteArray::fromHex("02000000"));
		err.clear();
		check(session_import::replay_snss(encrypted, &err).isEmpty(),
		      "an encrypted session is refused rather than read as rubbish");

		const QString profile = session_import::chromium_profile();
		const QString path    = session_import::chromium_session_path(profile);
		if (path.isEmpty()) {
			note("skipped the live replay: no Chromium profile on this machine.");
		} else {
			err.clear();
			int records = 0;
			const auto tabs = session_import::chromium_tabs(path, &err, &records);

			// **Assert what the reader is responsible for, which is not how
			// the person left their browser.** Whether any tab is still open
			// is their business: a session closed tab by tab and then quit is
			// a perfectly ordinary thing to find on disk, and this file used
			// to go red for it. What must hold is that the reader still
			// understands what this Chromium writes -- and that is exactly
			// the record count, because a moved command set yields a file
			// that walks cleanly and mentions no tabs at all.
			check(records > 0,
			      QString("a live session yields tab records (%1%2)")
			          .arg(records).arg(err.isEmpty() ? "" : ", " + err));

			if (records > 0 && tabs.isEmpty()) {
				// Reachable only through a working reader, which is what the
				// earlier version of this skip could not say for itself: it
				// was written against the guess that the tabs were open and
				// unreadable, so it never fired and the suite stayed red.
				// This one fires here, and names the count that proves the
				// replay ran.
				note(QString("skipped the open-tab checks: this Chromium was "
				              "left with nothing open (%1 tab record(s), all "
				              "closed).").arg(records));
			} else if (!tabs.isEmpty()) {
				// Plausibility, since there is nothing to diff against. A parser
				// that had the offsets wrong would produce mojibake and fragments
				// of other fields, not a list of addresses.
				int addressable = 0;
				for (const auto &t : tabs) {
					const QUrl u(t.url);
					if (u.isValid() && !u.scheme().isEmpty() && !t.title.isEmpty())
						++addressable;
				}
				check(addressable == tabs.size(),
				      QString("and every one is a valid address with a label "
				               "(%1 of %2)").arg(addressable).arg(tabs.size()));
				note(QString("first: %1  <%2>")
				         .arg(tabs.first().title.left(44), tabs.first().url.left(58)));
			}
		}
	}

	section("replaying a log the reader built itself");
	{
		// The properties that only a log can have, driven on one made here so
		// they are exercised whether or not a Chromium profile exists.
		auto cmd = [](quint8 id, const QByteArray &payload) {
			QByteArray out;
			const quint16 size = quint16(payload.size() + 1);
			out.append(char(size & 0xff)).append(char(size >> 8));
			out.append(char(id));
			out.append(payload);
			return out;
		};
		auto i32 = [](qint32 v) {
			QByteArray b(4, 0);
			memcpy(b.data(), &v, 4);
			return b;
		};
		auto nav = [&](qint32 tab, qint32 index, const QByteArray &url,
		               const QString &title) {
			QByteArray body;
			QByteArray inner = i32(tab) + i32(index);
			inner += i32(url.size()) + url;
			inner += QByteArray((4 - url.size() % 4) % 4, '\0');
			const QByteArray t16(reinterpret_cast<const char *>(title.utf16()),
			                      title.size() * 2);
			inner += i32(title.size()) + t16;
			inner += QByteArray((4 - t16.size() % 4) % 4, '\0');
			body = i32(inner.size()) + inner;   // the pickle's own size header
			return cmd(6, body);
		};

		QByteArray f;
		f.append(QByteArray::fromHex("534e5353"));
		f.append(QByteArray::fromHex("03000000"));
		f += cmd(0, i32(10) + i32(1));                   // tab 1 in window 10
		f += nav(1, 0, "https://one.test/a", "One A");
		f += nav(1, 1, "https://one.test/b", "One B");
		f += cmd(7, i32(1) + i32(1));                    // tab 1 is on entry 1
		f += cmd(0, i32(10) + i32(2));                   // tab 2 in window 10
		f += nav(2, 0, "https://two.test/", "Two");
		f += cmd(7, i32(2) + i32(0));

		QString err;
		auto tabs = session_import::replay_snss(f, &err);
		check(tabs.size() == 2,
		      QString("two tabs replay out (%1%2)").arg(tabs.size())
		          .arg(err.isEmpty() ? "" : ", " + err));
		// The property a document format would not have: the tab is at the
		// entry it selected, not the last one written.
		check(tabs.size() == 2 && tabs.at(0).url == "https://one.test/b",
		      QString("the first is on the entry it selected (%1)")
		          .arg(tabs.isEmpty() ? QString() : tabs.at(0).url));

		// **The past, not only the present.** Both formats carry every entry a
		// tab visited and both parsers used to keep one. Tab 1 was written with
		// two navigations and selected the second, so the whole list must come
		// through in the order Chromium indexed it, with the selected entry
		// found rather than assumed to be last.
		const bool one_ok = tabs.size() == 2 && tabs.at(0).history.entries.size() == 2;
		check(one_ok, QString("its history comes through, both entries (%1)")
		                  .arg(tabs.isEmpty() ? 0 : tabs.at(0).history.entries.size()));
		check(one_ok && tabs.at(0).history.entries.at(0).url == "https://one.test/a" &&
		          tabs.at(0).history.entries.at(1).url == "https://one.test/b",
		      "in navigation-index order, oldest first");
		check(one_ok && tabs.at(0).history.entries.at(0).title == "One A",
		      "carrying the titles, not only the addresses");
		check(one_ok && tabs.at(0).history.index == 1,
		      QString("and says which entry the tab was on (%1)")
		          .arg(one_ok ? tabs.at(0).history.index : -99));
		// The one-entry case, which must not report itself as having no
		// position: a tab that never navigated is still *on* its only entry.
		check(tabs.size() == 2 && tabs.at(1).history.entries.size() == 1 &&
		          tabs.at(1).history.index == 0,
		      "a tab with one entry is on that entry, not nowhere");

		// A tab closed later in the log is gone, however many navigations it
		// accumulated first. This is the whole reason the log has to be
		// replayed rather than scanned for urls.
		QByteArray with_close = f + cmd(16, i32(2) + i32(0) + i32(0) + i32(0));
		tabs = session_import::replay_snss(with_close, &err);
		check(tabs.size() == 1,
		      QString("a tab closed later in the log does not come back (%1)")
		          .arg(tabs.size()));
		check(tabs.size() == 1 && tabs.first().url.startsWith("https://one.test"),
		      "and the one that stayed is the one that stayed");

		// Same for a whole window.
		QByteArray with_window_close = f + cmd(17, i32(10) + i32(0) + i32(0) + i32(0));
		check(session_import::replay_snss(with_window_close, &err).isEmpty(),
		      "closing the window takes its tabs with it");

		// A half-written final record is normal, not corruption: the file is
		// being appended to by a running browser.
		QByteArray truncated = f;
		truncated.chop(5);
		check(!session_import::replay_snss(truncated, &err).isEmpty(),
		      "a truncated tail still yields the tabs before it");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
