// Does an assembled stream outlive the dialog that started it?
//
// `media_dialog` is opened with `exec()` from a stack object, and it used to
// own both the `hls_assembler` and the `QTemporaryDir` the assembled bytes were
// written into. So closing the picker destroyed the assembly part-way and
// removed the file underneath a player that had been handed the growing file
// and told, in the dialog's own words, that "playback continues locally".
//
// The property under test is therefore about a *lifetime*, and it is checked
// the way a user meets it: build the real dialog, click its real Watch button,
// wait for a segment to land, destroy the dialog, and then ask the filesystem.
//
// The CDN is in-process and answers each segment after a delay, because an
// assembly that has already finished cannot demonstrate surviving anything --
// a fixture fast enough to complete before the dialog closes would pass with
// the defect present.
#include "download_manager.h"
#include "hls_assembler.h"
#include "local_proxy.h"
#include "media_detector.h"
#include "media_dialog.h"
#include "mse_tap.h"
#include "player_launcher.h"
#include "stream_assembly.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <unistd.h>   // geteuid, for the check root cannot fail

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// One GET against the proxy, with an optional Range header exactly as written.
// The reply is read after spinning for the same reason `post_capture` does: the
// proxy answering lives in this process and needs the event loop to run.
static QByteArray fetch(const QUrl &url, const QByteArray &range = QByteArray()) {
	QTcpSocket s;
	s.connectToHost(url.host(), quint16(url.port()));
	if (!s.waitForConnected(3000))
		return "no connection";
	QByteArray req = "GET " + url.path().toUtf8() + " HTTP/1.1\r\n"
	                  "Host: " + url.host().toUtf8() + "\r\n";
	if (!range.isEmpty())
		req += "Range: " + range + "\r\n";
	req += "\r\n";
	s.write(req);
	s.waitForBytesWritten(3000);
	QByteArray all;
	QElapsedTimer t;
	t.start();
	while (t.elapsed() < 3000) {
		spin(60);
		all += s.readAll();
		if (s.state() == QAbstractSocket::UnconnectedState)
			break;
	}
	return all;
}

// The status line's code, and one header's value, from a whole response.
static int status_of(const QByteArray &response) {
	const QList<QByteArray> parts = response.left(64).split(' ');
	return parts.size() > 1 ? parts.at(1).toInt() : -1;
}
static QByteArray header_value(const QByteArray &response, const char *name) {
	const int head_end = response.indexOf("\r\n\r\n");
	const QByteArray head = head_end < 0 ? response : response.left(head_end);
	for (const QByteArray &line : head.split('\n')) {
		const QByteArray l = line.trimmed();
		if (l.startsWith(QByteArray(name) + ":"))
			return l.mid(int(strlen(name)) + 1).trimmed();
	}
	return {};
}
static QByteArray body_of(const QByteArray &response) {
	const int head_end = response.indexOf("\r\n\r\n");
	return head_end < 0 ? QByteArray() : response.mid(head_end + 4);
}

// One capture chunk, posted the way the injected script posts it. The reply
// is read after spinning rather than with waitForReadyRead, because the proxy
// answering lives in this same process and needs the event loop to run.
static QByteArray post_capture(const QUrl &url, const QByteArray &body) {
	QTcpSocket s;
	s.connectToHost(url.host(), quint16(url.port()));
	if (!s.waitForConnected(3000))
		return "no connection";
	QByteArray req = "POST " + url.path().toUtf8() + " HTTP/1.1\r\n"
	                  "Host: " + url.host().toUtf8() + "\r\n"
	                  "Content-Length: " + QByteArray::number(body.size()) +
	                  "\r\n\r\n" + body;
	s.write(req);
	s.waitForBytesWritten(3000);
	spin(250);
	return s.readAll();
}

// Six segments of distinguishable bytes, answered slowly enough that the
// dialog can be closed while the assembly is still running.
static constexpr int k_segments = 6;
static constexpr int k_seg_size = 4096;
static constexpr int k_delay_ms = 120;

static QByteArray segment_bytes(int i) {
	return QByteArray(k_seg_size, char('a' + i));
}
static QByteArray whole_stream() {
	QByteArray all;
	for (int i = 0; i < k_segments; ++i)
		all += segment_bytes(i);
	return all;
}

class slow_cdn : public QTcpServer {
public:
	quint16 port = 0;
	QHash<QString, QByteArray> files;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			const QString path = QString::fromUtf8(target);
			// The manifest answers at once; segments are what is paced, so
			// that the assembly is demonstrably mid-flight and not merely
			// slow to start.
			const int wait = path.endsWith(".m3u8") ? 0 : k_delay_ms;
			QTimer::singleShot(wait, s, [this, s, path] {
				if (!files.contains(path)) {
					s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
					          "Connection: close\r\n\r\n");
				} else {
					const QByteArray body = files.value(path);
					s->write("HTTP/1.1 200 OK\r\nContent-Type: "
					          "application/octet-stream\r\nContent-Length: " +
					          QByteArray::number(body.size()) +
					          "\r\nConnection: close\r\n\r\n" + body);
				}
				s->flush();
				s->disconnectFromHost();
			});
		});
	}
};

int main(int argc, char **argv) {
	QApplication app(argc, argv);

	slow_cdn cdn;
	if (!cdn.listen(QHostAddress::LocalHost, 0)) {
		std::printf("cannot listen: %s\n", qPrintable(cdn.errorString()));
		return 2;
	}
	cdn.port = cdn.serverPort();
	const QString base = QString("http://127.0.0.1:%1").arg(cdn.port);

	QByteArray manifest = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
	for (int i = 0; i < k_segments; ++i) {
		manifest += "#EXTINF:4.0,\n/seg" + QByteArray::number(i) + ".ts\n";
		cdn.files["/seg" + QString::number(i) + ".ts"] = segment_bytes(i);
	}
	manifest += "#EXT-X-ENDLIST\n";
	cdn.files["/live.m3u8"] = manifest;

	// A player that exists and does nothing. "Custom" is deliberately treated
	// as unable to take a manifest, which is exactly the branch that assembles.
	player_launcher players;
	players.refresh();
	players.set_selected(player_launcher::custom_id());
	players.set_custom_command("/bin/true %U");

	QTemporaryDir downloads_dir;
	download_manager downloads;
	downloads.set_directory(downloads_dir.path());

	local_proxy proxy;
	proxy.start();
	media_detector detector;
	mse_tap tap;

	media_item item;
	item.kind     = media_kind::hls;
	item.label    = "live.m3u8";
	item.url      = QUrl(base + "/live.m3u8");
	detector.add_item("example.invalid", item);

	stream_assembly assembly(&players, &downloads, &proxy, nullptr);
	QSignalSpy said(&assembly, &stream_assembly::status);

	section("an assembly outlives the dialog that started it");

	QString out;
	qint64  at_close = 0;
	int     said_at_close = 0;
	{
		media_dialog dlg(&detector, &players, &downloads, &proxy, &tap,
		                  &assembly, nullptr);
		dlg.set_site("example.invalid", "n1", stream_context{});

		QPushButton *watch = nullptr;
		for (QPushButton *b : dlg.findChildren<QPushButton *>())
			if (b->text().contains("Watch"))
				watch = b;
		check(watch && watch->isEnabled(),
		       "the dialog offers Watch on an HLS row");
		if (!watch) {
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return g_fail ? 1 : 0;
		}
		watch->click();

		// Wait for the first segment to be on disk, which is the moment the
		// player is handed the file and the moment the defect can bite.
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 8000) {
			out = assembly.output_path();
			if (!out.isEmpty() && QFileInfo(out).size() > 0)
				break;
			spin(25);
		}
		at_close      = QFileInfo(out).size();
		said_at_close = said.count();
		check(at_close > 0, "a segment lands while the dialog is open");
		check(assembly.running(),
		       "and the assembly is still running when it is closed");
	}

	// The dialog is gone. Everything below is about what it took with it.
	check(QFileInfo::exists(out),
	       "the assembled file survives the dialog closing");

	QElapsedTimer t;
	t.start();
	while (assembly.running() && t.elapsed() < 12000)
		spin(50);

	const qint64 final_size = QFileInfo(out).size();
	check(final_size > at_close,
	       "and goes on growing once nobody is watching it");
	check(said.count() > said_at_close,
	       "and goes on saying so, which is all the window has left to show");

	QFile f(out);
	QByteArray got;
	if (f.open(QIODevice::ReadOnly))
		got = f.readAll();
	check(got == whole_stream(),
	       "the whole stream is there, in order, with nothing repeated");

	// The scratch directory belongs to the assembly, so it is still there for
	// the next stream too -- a second Watch after the first dialog has closed
	// must have somewhere to write.
	check(!assembly.scratch_path().isEmpty() &&
	       QFileInfo(assembly.scratch_path()).isDir(),
	       "and the scratch directory is still there for the next one");

	section("a player that vanishes mid-stream does not take the proxy with it");

	// **Not a regression test, and labelled so it is not mistaken for one.**
	// It passes with the guards and without them, and an instrumented guard
	// never fired against it, so it does not demonstrate the hazard below --
	// it is a smoke test that a player vanishing mid-stream leaves the proxy
	// serving. Strengthening it means reaching the free, which nothing here
	// has managed.
	//
	// `serve_file` holds a raw pointer to the client
	// across `processEvents` and `waitForBytesWritten`, both of which run the
	// event loop, while `on_connection` wires `disconnected` to
	// `deleteLater()`. A player that closes mid-stream -- which is what one
	// does every time it probes or seeks -- destroyed the socket under the
	// loop, and the next `state()`, `write()` or `disconnectFromHost()` read
	// freed memory.
	//
	// **Driven by signals under one pump, and the first version of this was
	// not** -- which is the part of this worth keeping. Client and server share a thread here, so a sequential
	// `waitForReadyRead` lets `serve_file` run to completion inside it: the
	// abort then lands after the stream rather than during it, and the defect
	// is never touched. Worse, the parked client drains nothing, so the
	// server's `waitForBytesWritten` times out on every chunk and the test
	// deadlocks on flow control -- which is how that version failed against
	// the fixed code and gave the game away.
	//
	// A survived crash is not observable from inside the process, so the
	// assertion is that the proxy still serves afterwards: that says the loop
	// came out, not merely that this run happened not to fault.
	{
		QTemporaryDir served;
		const QString path = served.filePath("stream.bin");
		{
			QFile f(path);
			f.open(QIODevice::WriteOnly);
			f.write(QByteArray(256 * 1024, 'x'));   // several passes of the loop
		}

		local_proxy p2;
		check(p2.start(), "a proxy with a file to serve");
		const QUrl u = p2.publish_file(path, "application/octet-stream");
		check(u.isValid() && u.port() > 0, "which published it");

		const QByteArray req = ("GET " + u.path() + " HTTP/1.1\r\nHost: "
		                         + u.host() + "\r\n\r\n").toUtf8();
		auto pump = [](int ms) {
			QElapsedTimer t;
			t.start();
			while (t.elapsed() < ms)
				QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		};

		QTcpSocket first;
		bool gone = false;
		QObject::connect(&first, &QTcpSocket::connected,
		                  [&] { first.write(req); });
		QObject::connect(&first, &QTcpSocket::readyRead, [&] {
			// Inside the server's own processEvents: this is the moment the
			// socket is destroyed under the serving loop.
			if (!gone) {
				gone = true;
				first.abort();
			}
		});
		first.connectToHost(u.host(), quint16(u.port()));
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 5000 && !gone)
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		check(gone, "a player connects and goes away mid-stream");
		pump(1500);

		QTcpSocket second;
		QByteArray got;
		QObject::connect(&second, &QTcpSocket::connected,
		                  [&] { second.write(req); });
		QObject::connect(&second, &QTcpSocket::readyRead,
		                  [&] { got += second.readAll(); });
		second.connectToHost(u.host(), quint16(u.port()));
		t.restart();
		while (t.elapsed() < 8000 && !got.contains("200"))
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		check(got.contains("200"),
		       QString("and the proxy still serves afterwards (%1 bytes back)")
		         .arg(got.size()));
		second.abort();
		pump(800);
	}

	section("a retry pending from one assembly does not wake into the next");

	// **Press-scoped work outliving the press**, which is the same family as
	// the `Qt::UniqueConnection` defect above it: a failed segment schedules a
	// retry up to 1200 ms out, guarded by `if (!m_stopped)` -- and `start()`
	// sets that flag true through `stop()` and then straight back to false.
	// A retry pending from the previous press therefore passes its own guard
	// and drives the new run, racing its chain: both call `next_segment()`,
	// both advance `m_index`, and segments are duplicated or skipped.
	//
	// Asserted on the bytes rather than on a count, because the corruption is
	// in which segments landed and in what order.
	{
		// Stream A has a hole, so fetching it schedules a retry. Stream B is
		// whole. Disjoint bytes, so a mix-up is visible in the output.
		QByteArray want_b;
		cdn.files["/A0.ts"] = QByteArray(k_seg_size, 'p');
		// /A1.ts deliberately absent -> 404 -> retry scheduled
		QByteArray man_a = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		                    "#EXTINF:4.0,\n/A0.ts\n#EXTINF:4.0,\n/A1.ts\n"
		                    "#EXT-X-ENDLIST\n";
		cdn.files["/a.m3u8"] = man_a;

		QByteArray man_b = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
		for (int i = 0; i < 4; ++i) {
			const QByteArray seg(k_seg_size, char('q' + i));
			cdn.files["/B" + QString::number(i) + ".ts"] = seg;
			man_b += "#EXTINF:4.0,\n/B" + QByteArray::number(i) + ".ts\n";
			want_b += seg;
		}
		man_b += "#EXT-X-ENDLIST\n";
		cdn.files["/b.m3u8"] = man_b;

		QTemporaryDir work;
		const QString out_a = work.filePath("a.ts");
		const QString out_b = work.filePath("b.ts");

		auto pump = [](int ms) {
			QElapsedTimer t;
			t.start();
			while (t.elapsed() < ms)
				QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		};

		hls_assembler asmb(nullptr);
		bool b_done = false;
		QObject::connect(&asmb, &hls_assembler::completed,
		                  [&] { b_done = true; });

		stream_context ctx;
		asmb.start(QUrl(base + "/a.m3u8"), ctx, out_a);
		// A0 lands (120 ms), A1 answers 404 (120 ms), a retry is booked for
		// 400 ms after that. Start the next run inside that window.
		pump(350);
		asmb.start(QUrl(base + "/b.m3u8"), ctx, out_b);

		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 8000 && !b_done)
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		check(b_done, "the second assembly finishes");

		QFile f(out_b);
		f.open(QIODevice::ReadOnly);
		const QByteArray got = f.readAll();
		check(got == want_b,
		       QString("and holds exactly its own four segments (%1 of %2 bytes)")
		         .arg(got.size()).arg(want_b.size()));
		check(!got.contains('p'),
		       "with nothing from the assembly it replaced");
	}

	section("a capture the proxy cannot write is reported, not counted");
	{
		// The bytes were dropped and `received` simply stopped moving, so the
		// window's watchdog said "the page has stopped feeding its player" --
		// blaming the page for a file the browser could not write. Nothing
		// else in the path can tell those two apart, and the page keeps
		// posting either way because it is answered 204 regardless.
		//
		// `open_capture` refuses a path it cannot create, so the failure this
		// covers is the one that arrives AFTER a capture is open: a disk that
		// fills, a permission that changes, a removable volume unmounted.
		QTemporaryDir cdir;
		check(cdir.isValid(), "a scratch directory for the capture");
		local_proxy px;
		check(px.start(), "the proxy is listening");
		const QString cpath = QDir(cdir.path()).filePath("capture.bin");
		const QUrl curl = px.open_capture(cpath);
		check(!curl.isEmpty(), "and a capture is open");

		QStringList said;
		QObject::connect(&px, &local_proxy::failed,
		                  [&said](const QString &m) { said << m; });

		check(post_capture(curl, QByteArray(64, 'x')).startsWith("HTTP/1.1 204"),
		       "a chunk it can write is accepted");
		check(px.captured_bytes(curl) == 64, "and counted");
		check(said.isEmpty(), "with nothing reported");

		if (::geteuid() == 0) {
			std::printf("  --    running as root: a read-only file is still "
			             "writable, so the failed-write checks are skipped\n");
		} else {
			QFile::setPermissions(cpath, QFile::ReadOwner);
			post_capture(curl, QByteArray(64, 'y'));
			check(px.captured_bytes(curl) == 64,
			       "a chunk it cannot write is not counted");
			check(said.size() == 1, "and the failure is reported");
			check(said.size() == 1 && said.first().contains("Could not write"),
			       "naming the write rather than the page");
			// Once per capture, not once per POST: a page feeding a player
			// posts several times a second.
			post_capture(curl, QByteArray(64, 'z'));
			post_capture(curl, QByteArray(64, 'w'));
			check(said.size() == 1,
			       "said once for the capture, not once for every chunk");
			QFile::setPermissions(cpath, QFile::ReadOwner | QFile::WriteOwner);
		}
		px.close_capture(curl);
	}

	section("a capture that opens and then cannot be written is not counted");
	{
		// **The case the section above does not reach.** Making the file
		// read-only makes the *open* fail, which was always handled. What was
		// not is an open that succeeds and a write that does not: the write's
		// result was discarded and `received` advanced by what the page sent
		// rather than by what reached the disk. That removes the one symptom
		// either half has, because the reason a failure is reported at all is
		// that otherwise `received` stops and the watchdog blames the page.
		//
		// `/dev/full` is the portable way to ask for it on Linux: it opens,
		// accepts a truncate, and fails every write with ENOSPC -- a disk that
		// fills, without needing one.
		if (!QFileInfo("/dev/full").isWritable()) {
			std::printf("  --    no writable /dev/full here, so a write "
			             "that fails after a successful open cannot be "
			             "forced\n");
		} else {
			local_proxy fx;
			check(fx.start(), "the proxy is listening");
			const QUrl furl = fx.open_capture("/dev/full");
			check(!furl.isEmpty(), "and a capture opens on /dev/full");

			QStringList said;
			QObject::connect(&fx, &local_proxy::failed,
			                  [&said](const QString &m) { said << m; });

			check(post_capture(furl, QByteArray(64, 'x'))
			          .startsWith("HTTP/1.1 204"),
			       "the page is still answered 204, as it must be");
			check(fx.captured_bytes(furl) == 0,
			       QString("and nothing is counted, because nothing landed "
			                "(%1)").arg(fx.captured_bytes(furl)));
			check(said.size() == 1 && said.first().contains("Could not write"),
			       QString("with the write named rather than the page (%1)")
			           .arg(said.isEmpty() ? QString("nothing said")
			                                : said.first().left(40)));
			fx.close_capture(furl);
		}
	}

	section("a live playlist says so when it is saved, and a whole one does not");
	{
		// A playlist with no #EXT-X-ENDLIST is still growing, so running out of
		// segments is the end of the window it published rather than the end of
		// the stream. `hls_playlist` has parsed that all along as `is_live` and
		// nothing in src/ read it, so a captured thirty seconds of a broadcast
		// completed in exactly the same words as a whole film.
		//
		// The VOD control is the point of the section rather than decoration: a
		// note appended to every save would pass the first check on its own.
		QByteArray rolling = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
		for (int i = 0; i < 2; ++i)
			rolling += "#EXTINF:4.0,\n/seg" + QByteArray::number(i) + ".ts\n";
		cdn.files["/rolling.m3u8"] = rolling;   // and no ENDLIST

		auto save_and_listen = [&](const QString &label, const QString &path) {
			media_item it;
			it.kind  = media_kind::hls;
			it.label = label;
			it.url   = QUrl(base + path);
			auto *sa = new stream_assembly(&players, &downloads, &proxy, nullptr);
			QStringList lines;
			QObject::connect(sa, &stream_assembly::status,
			                  [&lines](const QString &t) { lines << t; });
			sa->save(it, stream_context{});
			QElapsedTimer el;
			el.start();
			while (sa->running() && el.elapsed() < 15000)
				spin(50);
			spin(600);   // the rewrap answers after the assembly has finished
			delete sa;
			return lines;
		};

		const QStringList live_said = save_and_listen("rolling.m3u8",
		                                              "/rolling.m3u8");
		const QStringList vod_said  = save_and_listen("live.m3u8", "/live.m3u8");

		check(!live_said.isEmpty() && !vod_said.isEmpty(),
		       "both saves reported something");
		check(live_said.filter("Live stream").size() > 0,
		       QString("the live one says what it captured (%1)")
		           .arg(live_said.join(" | ")));
		check(vod_said.filter("Live stream").size() == 0,
		       QString("and the complete one does not, so this is not a note on "
		                "every save (%1)").arg(vod_said.join(" | ")));
		check(live_said.filter("Saved").size() > 0,
		       "the live one still reports the file it wrote");

		// Watching is the other branch of the same ternary and has its own
		// wording, so it gets its own check rather than being assumed from the
		// save above: an untested branch of a two-armed message is exactly
		// where the wrong arm sits unnoticed.
		auto watch_and_listen = [&](const QString &label, const QString &path) {
			media_item it;
			it.kind  = media_kind::hls;
			it.label = label;
			it.url   = QUrl(base + path);
			auto *sa = new stream_assembly(&players, &downloads, &proxy, nullptr);
			QStringList lines;
			QObject::connect(sa, &stream_assembly::status,
			                  [&lines](const QString &t) { lines << t; });
			sa->watch(it, stream_context{});
			QElapsedTimer el;
			el.start();
			while (sa->running() && el.elapsed() < 15000)
				spin(50);
			spin(200);
			delete sa;
			return lines;
		};
		const QStringList live_watch = watch_and_listen("rolling.m3u8",
		                                               "/rolling.m3u8");
		const QStringList vod_watch  = watch_and_listen("live.m3u8", "/live.m3u8");
		check(live_watch.filter("Live stream").size() > 0,
		       QString("watching a live list says so too (%1)")
		           .arg(live_watch.join(" | ")));
		check(vod_watch.filter("Live stream").size() == 0 &&
		          vod_watch.filter("playback continues").size() > 0,
		       QString("and watching a complete one keeps the words it had (%1)")
		           .arg(vod_watch.join(" | ")));
	}

	section("what the proxy does with a Range header it cannot parse");
	{
		// **Every byte a player receives goes through this**, and a range is
		// the mechanism by which seeking works at all. The cases below are the
		// ones a hand-rolled parser gets wrong: `toLongLong()` answers 0 on a
		// value it cannot read and says so only through an `ok` flag nobody
		// passed it, so a malformed or multipart range came out as `0-0` and
		// the proxy served **one byte** with a 206 and a Content-Range saying
		// so. A player seeking into a stream is then told, truthfully, that it
		// received exactly what it was promised.
		QTemporaryDir rdir;
		check(rdir.isValid(), "a scratch directory to publish from");
		const QString rpath = QDir(rdir.path()).filePath("clip.bin");
		QByteArray payload;
		for (int i = 0; i < 1000; ++i)
			payload += char('a' + (i % 26));
		{
			QFile f(rpath);
			f.open(QIODevice::WriteOnly);
			f.write(payload);
		}

		local_proxy rpx;
		check(rpx.start(), "the proxy is listening");
		const QUrl url = rpx.publish_file(rpath, "video/mp2t");
		check(!url.isEmpty(), "and the file is published");

		const QByteArray whole = fetch(url);
		check(status_of(whole) == 200,
		      QString("no Range is 200 (%1)").arg(status_of(whole)));
		check(body_of(whole).size() == 1000,
		      QString("with the whole file (%1 bytes)").arg(body_of(whole).size()));

		const QByteArray first = fetch(url, "bytes=0-99");
		check(status_of(first) == 206,
		      QString("a plain range is 206 (%1)").arg(status_of(first)));
		check(header_value(first, "Content-Range") == "bytes 0-99/1000",
		      QString("with the range it served (%1)")
		          .arg(QString::fromUtf8(header_value(first, "Content-Range"))));
		check(body_of(first).size() == 100,
		      QString("and 100 bytes (%1)").arg(body_of(first).size()));

		const QByteArray openended = fetch(url, "bytes=900-");
		check(status_of(openended) == 206 &&
		          header_value(openended, "Content-Range") == "bytes 900-999/1000",
		      QString("an open-ended range runs to the end (%1)")
		          .arg(QString::fromUtf8(header_value(openended, "Content-Range"))));

		const QByteArray suffix = fetch(url, "bytes=-100");
		check(status_of(suffix) == 206 &&
		          header_value(suffix, "Content-Range") == "bytes 900-999/1000",
		      QString("a suffix range is the last hundred (%1)")
		          .arg(QString::fromUtf8(header_value(suffix, "Content-Range"))));

		const QByteArray past = fetch(url, "bytes=2000-");
		check(status_of(past) == 416,
		      QString("a range past the end is 416 (%1)").arg(status_of(past)));
		check(header_value(past, "Content-Range") == "bytes */1000",
		      QString("naming the size it has (%1)")
		          .arg(QString::fromUtf8(header_value(past, "Content-Range"))));

		// The two the parser was getting wrong. A multipart range is legal to
		// refuse and legal to answer with its first part; it is not legal to
		// answer with one byte and call it the first part. An unreadable value
		// is a syntactically invalid header, which RFC 7233 says to ignore --
		// so the whole file, 200, as if it had not been sent.
		const QByteArray multi = fetch(url, "bytes=0-99,200-299");
		check(status_of(multi) == 206,
		      QString("a multipart range is answered with one part (%1)")
		          .arg(status_of(multi)));
		check(header_value(multi, "Content-Range") == "bytes 0-99/1000",
		      QString("which is its first, not its first byte (%1)")
		          .arg(QString::fromUtf8(header_value(multi, "Content-Range"))));
		check(body_of(multi).size() == 100,
		      QString("and 100 bytes of it (%1)").arg(body_of(multi).size()));

		const QByteArray junk = fetch(url, "bytes=abc-def");
		check(status_of(junk) == 200,
		      QString("a range that cannot be read is ignored, not guessed "
		               "(%1)").arg(status_of(junk)));
		check(body_of(junk).size() == 1000,
		      QString("so the whole file is served (%1 bytes)")
		          .arg(body_of(junk).size()));
		check(header_value(junk, "Content-Range").isEmpty(),
		      "and no Content-Range is claimed for a range nobody asked for");

		const QByteArray half_junk = fetch(url, "bytes=100-nonsense");
		check(status_of(half_junk) == 200 && body_of(half_junk).size() == 1000,
		      QString("and half a range is the same answer (%1, %2 bytes)")
		          .arg(status_of(half_junk)).arg(body_of(half_junk).size()));
		rpx.unpublish_all();
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
