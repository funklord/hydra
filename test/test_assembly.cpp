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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
