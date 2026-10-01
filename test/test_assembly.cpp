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
	// **A path that answers differently the second time**, which is what a
	// growing live playlist is. Set `then[path]` and the first request gets
	// `files[path]` while every later one gets `then[path]`; without it a
	// fixture cannot express the only interesting property of a live list.
	QHash<QString, QByteArray> then;
	QHash<QString, int>        asked;
	// Which cookies each path's request carried, empty when it carried none.
	QHash<QString, QByteArray> cookie_seen;
	// An opt-in 302, to ask what the assembler and Qt do with a hop.
	QString    redirect_from;
	QByteArray redirect_to;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			const QString path = QString::fromUtf8(target);
			// **What came with it**, for the one question a fixture that only
			// counts requests cannot answer: which cookies a segment fetch
			// carried. Recorded per path, empty string when the header is
			// absent, so "no Cookie" and "never asked for" stay apart.
			cookie_seen[path] = QByteArray();
			for (const QByteArray &line : head.split('\n')) {
				const QByteArray l = line.trimmed();
				if (l.toLower().startsWith("cookie:"))
					cookie_seen[path] = l.mid(7).trimmed();
			}
			if (!redirect_from.isEmpty() && path.startsWith(redirect_from)) {
				++asked[path];
				s->write("HTTP/1.1 302 Found\r\nLocation: " + redirect_to +
				          "\r\nContent-Length: 0\r\n"
				          "Connection: close\r\n\r\n");
				s->flush();
				s->disconnectFromHost();
				return;
			}
			// The manifest answers at once; segments are what is paced, so
			// that the assembly is demonstrably mid-flight and not merely
			// slow to start.
			// A manifest answers at once whichever grammar it is in; segments
			// are what is paced, so that an assembly is demonstrably
			// mid-flight and not merely slow to start.
			const int wait = (path.endsWith(".m3u8") || path.endsWith(".mpd"))
			                   ? 0 : k_delay_ms;
			QTimer::singleShot(wait, s, [this, s, path] {
				if (!files.contains(path)) {
					s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
					          "Connection: close\r\n\r\n");
				} else {
					const int n = ++asked[path];
					const QByteArray body =
					  (n > 1 && then.contains(path)) ? then.value(path)
					                                 : files.value(path);
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
	// **The live stall bound, shortened so cases can reach it.** A live
	// playlist that stops growing ends the assembly after
	// `HYDRA_LIVE_STALL_MS`, thirty seconds by default -- the right answer for
	// a stream that is merely slow and impossible for a suite to wait out. Set
	// before anything runs, so the rolling-playlist section below exercises the
	// stall path rather than only the `#EXT-X-ENDLIST` one.
	qputenv("HYDRA_LIVE_STALL_MS", "1500");

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

	section("a live playlist is asked again, and its new segments are taken");
	{
		// **The window a live list happened to offer was the whole capture.**
		// sec 11.3 promises the tee-to-disk trick gives full backward seek over
		// everything captured; that was true only of one read of the playlist.
		// The list is re-read now and whatever it has grown is appended.
		cdn.files["/g1.ts"] = QByteArray("AAAA");
		cdn.files["/g2.ts"] = QByteArray("BBBB");
		cdn.files["/g3.ts"] = QByteArray("CCCC");
		cdn.files["/g4.ts"] = QByteArray("DDDD");
		// First read: two segments, still live. TARGETDURATION 1 so the poll
		// interval sits on its floor and the case takes seconds.
		cdn.files["/grow.m3u8"] =
		  QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:1\n"
		              "#EXT-X-MEDIA-SEQUENCE:0\n"
		              "#EXTINF:1,\n/g1.ts\n#EXTINF:1,\n/g2.ts\n");
		// Second read: two more, and an end so the case terminates on the
		// playlist rather than on the stall bound -- thirty seconds of no
		// growth is the right production answer and the wrong test.
		cdn.then["/grow.m3u8"] =
		  QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:1\n"
		              "#EXT-X-MEDIA-SEQUENCE:0\n"
		              "#EXTINF:1,\n/g1.ts\n#EXTINF:1,\n/g2.ts\n"
		              "#EXTINF:1,\n/g3.ts\n#EXTINF:1,\n/g4.ts\n"
		              "#EXT-X-ENDLIST\n");

		QTemporaryDir gdir;
		check(gdir.isValid(), "a scratch directory for the growing capture");
		const QString gout = QDir(gdir.path()).filePath("grow.ts");

		hls_assembler ga;
		QStringList gfail;
		bool gdone = false;
		QList<int> gsteps;
		QObject::connect(&ga, &hls_assembler::failed,
		                  [&gfail](const QString &m) { gfail << m; });
		QObject::connect(&ga, &hls_assembler::completed,
		                  [&gdone] { gdone = true; });
		QObject::connect(&ga, &hls_assembler::progress,
		                  [&gsteps](qint64, int d, int) { gsteps << d; });
		ga.start(QUrl(QString("http://127.0.0.1:%1/grow.m3u8").arg(cdn.port)),
		          stream_context{}, gout);
		for (int i = 0; i < 200 && !gdone && gfail.isEmpty(); ++i)
			spin(50);

		check(gdone && gfail.isEmpty(),
		       QString("the assembly completes (%1)")
		           .arg(gfail.isEmpty() ? QString("completed") : gfail.first()));
		const QByteArray got = [&gout] {
			QFile f(gout); f.open(QIODevice::ReadOnly); return f.readAll();
		}();
		check(got == QByteArray("AAAABBBBCCCCDDDD"),
		       QString("with the segments the second read added, in order (%1)")
		           .arg(QString::fromLatin1(got)));
		check(cdn.asked.value("/grow.m3u8") >= 2,
		       QString("the playlist really was asked again (%1 times)")
		           .arg(cdn.asked.value("/grow.m3u8")));
		// **The two already-taken segments are not fetched twice.** The second
		// playlist lists all four; what makes the first two old is the media
		// sequence, not their absence.
		check(cdn.asked.value("/g1.ts") == 1 && cdn.asked.value("/g2.ts") == 1,
		       QString("and the first two are not re-fetched (%1, %2)")
		           .arg(cdn.asked.value("/g1.ts"))
		           .arg(cdn.asked.value("/g2.ts")));
		bool climbs = true;
		for (int i = 1; i < gsteps.size(); ++i)
			if (gsteps[i] < gsteps[i - 1])
				climbs = false;
		check(gsteps.size() >= 4 && climbs,
		       QString("progress climbs across the re-poll rather than "
		                "restarting (%1 reports)").arg(gsteps.size()));
		check(!ga.was_live(),
		       "and the capture knows the list ended, since the second read "
		       "carried ENDLIST");
	}

	section("the dialog routes a DASH row to the assembler, as it does HLS");
	{
		// **The condition that makes any of this reachable.** The dialog sent
		// HLS to the assembler and handed every other manifest straight to the
		// player, so an MPD reached the assembler from nowhere -- the engine
		// could take one and nothing gave it one. Driven through the button
		// rather than the private method, which is how the section above
		// drives Watch and is the only door a person has.
		cdn.files["/init.mp4"] = QByteArray("INIT");
		cdn.files["/s1.m4s"]   = QByteArray("ONE");
		cdn.files["/s2.m4s"]   = QByteArray("TWO");
		cdn.files["/routed.mpd"] =
		  QByteArray("<?xml version=\"1.0\"?>\n"
		              "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		              "  <AdaptationSet mimeType=\"video/mp4\">\n"
		              "    <Representation id=\"v\" bandwidth=\"1\">\n"
		              "      <SegmentList>\n"
		              "        <Initialization sourceURL=\"/init.mp4\"/>\n"
		              "        <SegmentURL media=\"/s1.m4s\"/>\n"
		              "        <SegmentURL media=\"/s2.m4s\"/>\n"
		              "      </SegmentList>\n"
		              "    </Representation>\n"
		              "  </AdaptationSet></Period></MPD>\n");

		media_detector det2;
		media_item mpd;
		mpd.kind  = media_kind::dash;
		mpd.label = "routed.mpd";
		mpd.url   = QUrl(base + "/routed.mpd");
		det2.add_item("example.invalid", mpd);

		stream_assembly asm2(&players, &downloads, &proxy, nullptr);
		media_dialog dlg2(&det2, &players, &downloads, &proxy, &tap,
		                   &asm2, nullptr);
		dlg2.set_site("example.invalid", "n2", stream_context{});

		QPushButton *w = nullptr;
		for (QPushButton *b : dlg2.findChildren<QPushButton *>())
			if (b->text().contains("Watch"))
				w = b;
		check(w && w->isEnabled(),
		       "a DASH row offers Watch with a player that reads no manifests");
		if (w) {
			w->click();
			for (int i = 0; i < 120 && !asm2.running() &&
			                  asm2.output_path().isEmpty(); ++i)
				spin(50);
			check(!asm2.output_path().isEmpty(),
			       QString("and pressing it starts an assembly rather than "
			                "handing the MPD to the player (%1)")
			           .arg(asm2.output_path().isEmpty() ? QString("nothing")
			                                              : QString("started")));
		}
	}

	section("an MPD is assembled through the same engine, or refused by name");
	{
		// **The engine below the manifest is general over an ordered list of
		// segment URLs**, and a DASH representation is exactly that plus an
		// initialisation segment that goes first -- so DASH converts into the
		// same list rather than getting a second assembler. These drive
		// `hls_assembler` directly, because the manifest grammar is what is
		// under test and `stream_assembly` adds a player and a proxy to it.
		const QString base = QString("http://127.0.0.1:%1").arg(cdn.port);
		cdn.files["/init.mp4"]  = QByteArray("INIT");
		cdn.files["/s1.m4s"]    = QByteArray("ONE");
		cdn.files["/s2.m4s"]    = QByteArray("TWO");
		cdn.files["/s3.m4s"]    = QByteArray("THREE");
		cdn.files["/one.mpd"]   =
		  QByteArray("<?xml version=\"1.0\"?>\n"
		              "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		              "  <AdaptationSet mimeType=\"video/mp4\">\n"
		              "    <Representation id=\"v\" bandwidth=\"1\">\n"
		              "      <SegmentList>\n"
		              "        <Initialization sourceURL=\"/init.mp4\"/>\n"
		              "        <SegmentURL media=\"/s1.m4s\"/>\n"
		              "        <SegmentURL media=\"/s2.m4s\"/>\n"
		              "      </SegmentList>\n"
		              "    </Representation>\n"
		              "  </AdaptationSet></Period></MPD>\n");

		QTemporaryDir odir;
		check(odir.isValid(), "a scratch directory to assemble into");
		const QString out = QDir(odir.path()).filePath("dash.mp4");

		hls_assembler as;
		QStringList failures;
		bool done = false;
		QObject::connect(&as, &hls_assembler::failed,
		                  [&failures](const QString &m) { failures << m; });
		QObject::connect(&as, &hls_assembler::completed,
		                  [&done] { done = true; });
		as.start(QUrl(base + "/one.mpd"), stream_context{}, out);
		// **`spin` rather than `processEvents`**: the latter returns as soon as
		// the queue is empty, so a loop of 400 of them finished in milliseconds
		// and every check below it read a state that had not happened yet. Five
		// of them failed that way before this line was right, which is a
		// fixture fault failing loudly rather than a check passing vacuously.
		for (int i = 0; i < 120 && !done && failures.isEmpty(); ++i)
			spin(50);

		check(done && failures.isEmpty(),
		       QString("an MPD with one video stream assembles (%1)")
		           .arg(failures.isEmpty() ? QString("completed") : failures.first()));
		const QByteArray got = [&out] {
			QFile f(out); f.open(QIODevice::ReadOnly); return f.readAll();
		}();
		check(got == QByteArray("INITONETWO"),
		       QString("with the init segment first and the media in order "
		                "(%1)").arg(QString::fromLatin1(got)));

		// **Refused rather than assembled silently.** Video without its audio
		// plays perfectly and says nothing, which is the one outcome worse
		// than refusing.
		cdn.files["/split.mpd"] =
		  QByteArray("<?xml version=\"1.0\"?>\n"
		              "<MPD mediaPresentationDuration=\"PT4S\"><Period>\n"
		              "  <AdaptationSet mimeType=\"video/mp4\">\n"
		              "    <Representation id=\"v\" bandwidth=\"1\">\n"
		              // **Two video segments against one audio segment, on
		              // purpose.** With one each, an index that restarted per
		              // list would go 1 then 1 -- never decreasing -- so the
		              // monotonic check could not fail. Two then one makes it
		              // drop from 2 to 1 if the offset is ever lost.
		              "      <SegmentList><SegmentURL media=\"/s1.m4s\"/>"
		              "<SegmentURL media=\"/s2.m4s\"/></SegmentList>\n"
		              "    </Representation></AdaptationSet>\n"
		              "  <AdaptationSet mimeType=\"audio/mp4\">\n"
		              "    <Representation id=\"a\" bandwidth=\"1\">\n"
		              "      <SegmentList><SegmentURL media=\"/s3.m4s\"/>"
		              "</SegmentList>\n"
		              "    </Representation></AdaptationSet>\n"
		              "</Period></MPD>\n");
		hls_assembler as2;
		QStringList f2;
		bool done2 = false;
		QObject::connect(&as2, &hls_assembler::failed,
		                  [&f2](const QString &m) { f2 << m; });
		QObject::connect(&as2, &hls_assembler::completed,
		                  [&done2] { done2 = true; });
		as2.start(QUrl(base + "/split.mpd"), stream_context{},
		           QDir(odir.path()).filePath("split.mp4"));
		for (int i = 0; i < 120 && !done2 && f2.isEmpty(); ++i)
			spin(50);
		check(!done2 && f2.size() == 1,
		       QString("separate audio is refused rather than assembled (%1)")
		           .arg(done2 ? QString("it completed") : QString("refused")));
		check(f2.size() == 1 && f2.first().contains("audio"),
		       QString("naming the audio as what is missing (%1)")
		           .arg(f2.isEmpty() ? QString("nothing said") : f2.first().left(48)));

		// Neither grammar: refused rather than parsed as the wrong one.
		cdn.files["/plain.txt"] = QByteArray("this is not a manifest");
		hls_assembler as3;
		QStringList f3;
		QObject::connect(&as3, &hls_assembler::failed,
		                  [&f3](const QString &m) { f3 << m; });
		as3.start(QUrl(base + "/plain.txt"), stream_context{},
		           QDir(odir.path()).filePath("plain.bin"));
		for (int i = 0; i < 120 && f3.isEmpty(); ++i)
			spin(50);
		check(f3.size() == 1 && f3.first().contains("neither"),
		       QString("a body in neither grammar is refused (%1)")
		           .arg(f3.isEmpty() ? QString("nothing said") : f3.first().left(48)));

		// An MPD the parser refuses is refused here too, with its reason.
		cdn.files["/bad.mpd"] =
		  QByteArray("<?xml version=\"1.0\"?>\n"
		              "<MPD><Period><AdaptationSet mimeType=\"video/mp4\">\n"
		              "  <SegmentTemplate duration=\"2\" timescale=\"1\""
		              " media=\"/s$Number$.m4s\"/>\n"
		              "  <Representation id=\"v\" bandwidth=\"1\"/>\n"
		              "</AdaptationSet></Period></MPD>\n");
		hls_assembler as4;
		QStringList f4;
		QObject::connect(&as4, &hls_assembler::failed,
		                  [&f4](const QString &m) { f4 << m; });
		as4.start(QUrl(base + "/bad.mpd"), stream_context{},
		           QDir(odir.path()).filePath("bad.bin"));
		for (int i = 0; i < 120 && f4.isEmpty(); ++i)
			spin(50);
		check(f4.size() == 1 && f4.first().contains("MPD not understood"),
		       QString("and an MPD the parser refuses carries its reason (%1)")
		           .arg(f4.isEmpty() ? QString("nothing said") : f4.first().left(48)));

		// **Given somewhere to put it, the audio is assembled too.** Same
		// engine, a second pass, a second file -- which is what makes a mux
		// possible, and what Save asks for where Watch cannot.
		hls_assembler as5;
		QStringList f5;
		bool done5 = false;
		QList<QPair<int, int>> steps;   // (done, total) as reported
		QObject::connect(&as5, &hls_assembler::failed,
		                  [&f5](const QString &m) { f5 << m; });
		QObject::connect(&as5, &hls_assembler::completed,
		                  [&done5] { done5 = true; });
		QObject::connect(&as5, &hls_assembler::progress,
		                  [&steps](qint64, int d, int t) { steps << qMakePair(d, t); });
		const QString v_out = QDir(odir.path()).filePath("split-v.mp4");
		const QString a_out = v_out + ".audio";
		as5.start(QUrl(base + "/split.mpd"), stream_context{}, v_out, a_out);
		for (int i = 0; i < 120 && !done5 && f5.isEmpty(); ++i)
			spin(50);

		check(done5 && f5.isEmpty(),
		       QString("an MPD with separate audio assembles when asked (%1)")
		           .arg(f5.isEmpty() ? QString("completed") : f5.first()));
		check(as5.audio_path() == a_out,
		       QString("and says where the audio went (%1)")
		           .arg(as5.audio_path().isEmpty() ? QString("nowhere")
		                                            : QString("named")));
		const auto slurp = [](const QString &path) {
			QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
		};
		check(slurp(v_out) == QByteArray("ONETWO"),
		       QString("the video file holds both video segments (%1)")
		           .arg(QString::fromLatin1(slurp(v_out))));
		check(slurp(a_out) == QByteArray("THREE"),
		       QString("and the audio file only the audio one, not all three "
		                "in one (%1)").arg(QString::fromLatin1(slurp(a_out))));

		// **Progress over the whole job, not the current list.** Each pass
		// replaces the segment list, so a bar fed the list's own index would
		// reach the end of the video, drop to zero and climb again -- which
		// reads as a restarted download. Monotonic and one total throughout.
		bool monotonic = true, one_total = true;
		for (int i = 1; i < steps.size(); ++i) {
			if (steps[i].first < steps[i - 1].first)
				monotonic = false;
			if (steps[i].second != steps[0].second)
				one_total = false;
		}
		check(steps.size() >= 2 && monotonic,
		       QString("progress never walks backwards across the two passes "
		                "(%1 reports)").arg(steps.size()));
		check(one_total && !steps.isEmpty() && steps[0].second == 3,
		       QString("and the total is the whole job throughout (%1)")
		           .arg(steps.isEmpty() ? QString("none")
		                                 : QString::number(steps[0].second)));
	}

	// **Every fetch the assembler makes asked about the page, not about itself.**
	// A manifest, its segments and a separate audio track can sit on different
	// hosts, and `stream_context::cookies` was filled once from the page's own
	// address -- so a page's session cookie went out with every segment request
	// wherever it went. It asks the resolver about the url being fetched now.
	section("a segment fetch carries only cookies that belong to its host");
	{
		cdn.cookie_seen.clear();
		QByteArray man = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n";
		for (int i = 0; i < 2; ++i) {
			cdn.files["/C" + QString::number(i) + ".ts"] =
			  QByteArray(k_seg_size, char('a' + i));
			man += "#EXTINF:4.0,\n/C" + QByteArray::number(i) + ".ts\n";
		}
		man += "#EXT-X-ENDLIST\n";
		cdn.files["/c.m3u8"] = man;

		QTemporaryDir work;
		hls_assembler as;
		bool done = false;
		QObject::connect(&as, &hls_assembler::completed, [&] { done = true; });

		// A resolver that behaves like a cookie jar: the page's cookies for the
		// page's host, nothing for anyone else. The fixture is on 127.0.0.1.
		QList<QUrl> asked;
		stream_context ctx;
		ctx.referer = "https://site.example/watch/1";
		ctx.cookies_for = [&asked](const QUrl &to) {
			asked << to;
			return to.host() == "site.example" ? QString("sid=abc123")
			                                    : QString();
		};
		as.start(QUrl(base + "/c.m3u8"), ctx, work.filePath("c.ts"));
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 8000 && !done)
			spin(50);
		check(done, "the assembly finished");

		// Asked about each url it fetched, rather than once about the page.
		check(asked.size() >= 3,
		      QString("the resolver was asked per fetch (%1 time(s))")
		          .arg(asked.size()));
		bool only_fetched = !asked.isEmpty();
		for (const QUrl &u : asked)
			if (u.host() != "127.0.0.1")
				only_fetched = false;
		check(only_fetched,
		      "and always about the host being fetched, never about the page");

		// **The check the old shape could not pass.** Every path the fixture
		// served saw no Cookie at all, where a page-filled field sent
		// `sid=abc123` to all three.
		QStringList leaked;
		for (auto it = cdn.cookie_seen.cbegin(); it != cdn.cookie_seen.cend();
		     ++it)
			if (!it.value().isEmpty())
				leaked << it.key() + "=" + QString::fromUtf8(it.value());
		check(leaked.isEmpty(),
		      QString("no fetch carried the page's cookies (%1)")
		          .arg(leaked.isEmpty() ? QStringLiteral("none did")
		                                 : leaked.join(", ")));

		// And a named header still goes out, which is what keeps the check
		// above from passing for an assembler that sends no cookies ever.
		cdn.cookie_seen.clear();
		bool done2 = false;
		hls_assembler named;
		QObject::connect(&named, &hls_assembler::completed,
		                  [&] { done2 = true; });
		stream_context with;
		with.cookies = "named=by-extractor";
		named.start(QUrl(base + "/c.m3u8"), with, work.filePath("c2.ts"));
		t.restart();
		while (t.elapsed() < 8000 && !done2)
			spin(50);
		check(done2 &&
		        cdn.cookie_seen.value("/c.m3u8") == "named=by-extractor",
		      QString("an extractor's named Cookie still goes out (%1)")
		          .arg(QString::fromUtf8(cdn.cookie_seen.value("/c.m3u8"))));
	}

	// **What Qt carries across a hop that the assembler did not ask about.**
	// `hls_assembler` sets `Cookie` for the url it is fetching and then lets
	// Qt follow a redirect, and Qt builds the follow-up from the original
	// request's raw headers. If it carries the Cookie over, a hop onto another
	// host sends a cookie that host never set -- the leak the per-url resolver
	// closed, by a route the resolver is never consulted on.
	//
	// Measured rather than argued, and on loopback both ends are 127.0.0.1, so
	// what this can show is the *carry-over* and not a cross-host send. That is
	// the mechanism; the host argument follows from it.
	section("what a redirect carries that nobody was asked about");
	{
		cdn.cookie_seen.clear();
		cdn.asked.clear();
		QByteArray man = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		                  "#EXTINF:4.0,\n/D0.ts\n#EXT-X-ENDLIST\n";
		cdn.files["/d.m3u8"]  = man;
		cdn.files["/D0.ts"]   = QByteArray(k_seg_size, 'd');
		cdn.redirect_from = "/hop";
		cdn.redirect_to   = "/d.m3u8";

		QTemporaryDir work;
		hls_assembler as;
		bool done = false;
		QObject::connect(&as, &hls_assembler::completed, [&] { done = true; });

		QStringList asked_about;
		stream_context ctx;
		ctx.referer = "https://site.example/watch/1";
		ctx.cookies_for = [&asked_about](const QUrl &to) {
			asked_about << to.path();
			// Answer only for the url the hop starts from, so anything the
			// second request carries was carried rather than asked for.
			return to.path().startsWith("/hop") ? QString("hop=1") : QString();
		};
		as.start(QUrl(base + "/hop/signed"), ctx, work.filePath("d.ts"));
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 8000 && !done)
			spin(50);

		std::printf("  ..    asked about %s; /d.m3u8 saw cookie '%s'\n",
		             qPrintable(asked_about.join(", ")),
		             qPrintable(QString::fromUtf8(
		               cdn.cookie_seen.value("/d.m3u8"))));

		check(done, "the assembly finished through the redirect");
		check(asked_about.contains("/hop/signed"),
		      "the resolver was asked about the url the hop starts from");
		check(!asked_about.contains("/d.m3u8"),
		      "and never about the url Qt followed to, which is the point");
		// **Qt carries it, measured, and within one origin that is correct.**
		// The cookie belongs to that host whichever path is asked for, so this
		// records the behaviour rather than objecting to it. What must not
		// happen is the same carry across origins, which the next block asks.
		check(cdn.cookie_seen.value("/d.m3u8") == QByteArray("hop=1"),
		      QString("a same-origin hop carries it, which is where it belongs "
		               "(%1)")
		          .arg(QString::fromUtf8(cdn.cookie_seen.value("/d.m3u8"))));
	}

	// **And a hop to another origin is refused rather than followed**, because
	// Qt would carry that same Cookie to a host that never set it. `localhost`
	// and `127.0.0.1` are different origins to Qt while being the same machine,
	// which is what makes this measurable on loopback at all.
	section("a redirect to another origin is not followed");
	{
		cdn.cookie_seen.clear();
		cdn.files["/e.m3u8"] = QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:4\n"
		                                   "#EXTINF:4.0,\n/D0.ts\n"
		                                   "#EXT-X-ENDLIST\n");
		cdn.redirect_from = "/cross";
		cdn.redirect_to   = QByteArray("http://localhost:") +
		                     QByteArray::number(cdn.port) + "/e.m3u8";

		QTemporaryDir work;
		hls_assembler as;
		bool done = false;
		QString why;
		QObject::connect(&as, &hls_assembler::completed, [&] { done = true; });
		QObject::connect(&as, &hls_assembler::failed,
		                  [&why](const QString &e) { why = e; });

		stream_context ctx;
		ctx.cookies_for = [](const QUrl &to) {
			return to.path().startsWith("/cross") ? QString("hop=1")
			                                       : QString();
		};
		as.start(QUrl(base + "/cross/signed"), ctx, work.filePath("e.ts"));
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < 8000 && !done && why.isEmpty())
			spin(50);

		check(!done && !why.isEmpty(),
		      QString("the assembly stops and says why (%1)")
		          .arg(why.isEmpty() ? QStringLiteral("(said nothing)") : why));
		check(!cdn.cookie_seen.contains("/e.m3u8"),
		      QString("and the other origin was never asked at all (%1)")
		          .arg(cdn.cookie_seen.contains("/e.m3u8")
		                 ? QString::fromUtf8(cdn.cookie_seen.value("/e.m3u8"))
		                 : QStringLiteral("not asked")));
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
		//
		// **And this now exercises the live stall bound**, which nothing did
		// before. `/rolling.m3u8` never grows, so re-polling finds nothing and
		// the assembly ends on the bound -- reachable here only because
		// `HYDRA_LIVE_STALL_MS` is set at the top of this file. Before
		// re-polling existed, running out of segments simply ended the
		// assembly and there was no bound to reach.
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
