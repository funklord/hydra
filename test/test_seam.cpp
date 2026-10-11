#include "download_manager.h"
#include "http_download_source.h"
#include "fake_sources.h"

#include <QCoreApplication>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtGlobal>

#include <cstdio>

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const QString &what) {
	if (ok) {
		++g_pass;
		std::printf("  ok    %s\n", qPrintable(what));
	} else {
		++g_fail;
		std::printf("  FAIL  %s\n", qPrintable(what));
	}
}

static void section(const char *name) { std::printf("\n== %s ==\n", name); }

// Let queued signals settle without a fixed sleep.
static void spin(int ms = 50) {
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

static const download_job *job_by_id(const download_manager &m, int id) {
	for (const download_job &j : m.jobs())
		if (j.id == id)
			return &j;
	return nullptr;
}

// Null-safe: a refused enqueue should fail a check, not crash the run.
static download_state status_of(const download_manager &m, int id) {
	const download_job *j = job_by_id(m, id);
	return j ? j->status : download_state::failed;
}

// ---------------------------------------------------------------- routing --
static void test_routing() {
	section("routing: the manager picks a source without naming a transport");

	download_manager m;
	auto *http = new http_download_source;
	auto *tor  = new fake_torrent_source;
	m.add_source(http);
	m.add_source(tor);

	check(m.source_for(QUrl("https://e.example/a.mp4")) == http,
	      "an http file goes to the http source");
	check(m.source_for(QUrl("magnet:?xt=urn:btih:abc")) == tor,
	      "a magnet link goes to the torrent source");
	check(m.source_for(QUrl("https://e.example/x.torrent")) == tor,
	      "a .torrent link goes to the torrent source, not http");
	check(m.source_for(QUrl("gopher://e.example/f")) == nullptr,
	      "an unknown scheme finds no source");

	QString err;
	check(m.enqueue(QUrl("gopher://e.example/f"), QString(), &err) == 0,
	      "enqueueing an unroutable URL is refused");
	check(!err.isEmpty(), "refusal carries a reason");

	// The specific reason must survive, not be flattened to a generic one.
	//
	// **Anchored on "Find Media" rather than on the old wording.** This read
	// `err.contains("Watch in player")`, which pinned a sentence claiming
	// segment assembly was not implemented -- true when written and false
	// since DASH was assembled, so the check was keeping a stale claim alive
	// rather than keeping the specific reason alive. The anchor is now the
	// route a person can actually take, which is also the part that
	// distinguishes this refusal from the generic one below.
	err.clear();
	m.enqueue(QUrl("https://e.example/stream.m3u8"), QString(), &err);
	check(err.contains("Find Media") && err.contains("playlist"),
	      "the http source's own HLS message reaches the caller");

	err.clear();
	m.enqueue(QUrl("https://e.example/page.html"), QString(), &err);
	check(err.contains("saveable"), "non-saveable URLs get the http source's reason");

	check(m.source_by_id("torrent") == tor, "sources are findable by id");
}

// ---------------------------------------------------------------- consent --
static void test_consent() {
	section("consent: a publicly-observable source cannot start unasked");

	download_manager m;
	auto *tor = new fake_torrent_source;
	m.add_source(tor);

	QSignalSpy asked(&m, &download_manager::consent_required);

	QString err;
	const int id = m.enqueue(QUrl("magnet:?xt=urn:btih:abc"), "node-1", &err);
	check(id != 0, "the job is accepted");
	check(asked.count() == 1, "consent_required is emitted once");
	check(asked.value(0).value(1).toString().contains("announces your address"),
	      "the note explains what is publicly observable");

	const download_job *j = job_by_id(m, id);
	check(j && j->status == download_state::queued,
	      "the job waits in queued rather than starting");
	check(!tor->live(id), "the source was never asked to start it");
	check(j && j->public_participation,
	      "the job is marked so the UI can show the row differently");

	m.set_consent("torrent", true);
	j = job_by_id(m, id);
	check(tor->live(id), "granting consent releases the job to the source");
	check(j && j->status == download_state::resolving,
	      "and the source's own first state wins over a presumed 'running'");
	check(m.has_consent("torrent"), "consent is remembered");

	// A second job must not re-ask.
	QSignalSpy asked_again(&m, &download_manager::consent_required);
	m.enqueue(QUrl("magnet:?xt=urn:btih:def"), QString(), &err);
	check(asked_again.count() == 0, "consent is not asked twice");

	// Revoking must not retro-kill a running job, but must gate the next one.
	m.set_consent("torrent", false);
	check(tor->live(id), "revoking does not abort a job already running");
	QSignalSpy asked_third(&m, &download_manager::consent_required);
	const int id3 = m.enqueue(QUrl("magnet:?xt=urn:btih:ghi"), QString(), &err);
	check(asked_third.count() == 1, "revoking makes the next job ask again");
	check(!tor->live(id3), "and that job waits");
}

// ------------------------------------------------------- torrent lifecycle --
static void test_torrent_lifecycle() {
	section("lifecycle: the states HTTP never enters");

	download_manager m;
	auto *tor = new fake_torrent_source;
	m.add_source(tor);
	m.set_consent("torrent", true);

	QString err;
	const int id = m.enqueue(QUrl("magnet:?xt=urn:btih:abc"), "node-7", &err);

	const download_job *j = job_by_id(m, id);
	check(j->status == download_state::resolving, "a magnet starts in resolving");
	check(j->total == -1, "with no total size");
	check(j->detail == "fetching metadata", "and the source's own words");
	check(j->path.isEmpty(), "and no filename yet");

	tor->resolve(id, 4000, { {"season/e01.mkv", 2000}, {"season/e02.mkv", 1900},
		                        {"season/readme.txt", 100} });
	j = job_by_id(m, id);
	check(j->status == download_state::running, "metadata moves it to running");
	check(j->total == 4000, "the size appears late, and is taken");
	check(j->files.size() == 3, "a job can be several files");
	check(j->path == "/downloads/season/e01.mkv", "with a primary path for the UI");
	check(j->files.first().size == 2000, "and per-file sizes, so the UI can choose");

	tor->advance(id, 2000, 4000);
	j = job_by_id(m, id);
	check(j->received == 2000, "progress updates");
	check(!j->complete(), "half a torrent is not complete");

	tor->complete_into_seeding(id, 4000);
	j = job_by_id(m, id);
	check(j->status == download_state::seeding, "completion lands in seeding");
	check(j->complete(), "seeding counts as complete — the file is usable");
	check(!j->terminal(), "but is not terminal — the source is still working");
	check(tor->live(id), "and the source has not let go");
	check(j->received == j->total, "with every byte accounted for");

	tor->stop_seeding(id);
	j = job_by_id(m, id);
	check(j->status == download_state::done, "stopping the seed finishes the job");
	check(j->terminal(), "which is terminal");
	check(j->complete(), "and still complete");
}

// ------------------------------------------------------------ concurrency --
static void test_concurrency() {
	section("concurrency: per source, not global");

	download_manager m;
	auto *tor = new fake_torrent_source;      // max_concurrent = 3
	m.add_source(tor);
	m.set_consent("torrent", true);

	QString err;
	QList<int> ids;
	for (int i = 0; i < 5; ++i)
		ids << m.enqueue(QUrl(QString("magnet:?xt=urn:btih:%1").arg(i)), QString(), &err);

	int live = 0, queued = 0;
	for (int id : ids) {
		if (tor->live(id))
			++live;
		if (job_by_id(m, id)->status == download_state::queued)
			++queued;
	}
	check(live == 3, "three torrents run at once, as the source declares");
	check(queued == 2, "the rest wait");

	// A seeding torrent still holds its slot: it is still doing work.
	tor->complete_into_seeding(ids[0], 100);
	int live_after = 0;
	for (int id : ids)
		if (tor->live(id))
			++live_after;
	check(live_after == 3, "a seeding job still occupies its slot");

	tor->stop_seeding(ids[0]);
	live_after = 0;
	for (int id : ids)
		if (tor->live(id))
			++live_after;
	check(live_after == 3, "releasing one starts the next");
	check(job_by_id(m, ids[3])->status != download_state::queued,
	      "specifically the next in order");
	check(job_by_id(m, ids[0])->status == download_state::done,
	      "and the released one is done");
}

static void test_independent_sources() {
	section("concurrency: one source's queue does not block another's");

	download_manager m;
	auto *http = new http_download_source;    // max_concurrent = 1
	auto *tor  = new fake_torrent_source;
	m.add_source(http);
	m.add_source(tor);
	m.set_consent("torrent", true);

	QString err;
	// An http job that will fail to start (unroutable host is fine; it still
	// occupies the single http slot until the reply errors).
	const int h = m.enqueue(QUrl("http://127.0.0.1:9/big.mp4"), QString(), &err);
	const int t = m.enqueue(QUrl("magnet:?xt=urn:btih:abc"), QString(), &err);

	check(h != 0 && t != 0, "both are accepted");
	check(tor->live(t), "the torrent starts even though the http slot is busy");
	check(job_by_id(m, t)->source_id == "torrent", "and is attributed correctly");
	check(job_by_id(m, h)->source_id == "http", "as is the http job");
}

// ----------------------------------------------------------------- cancel --
static void test_cancel_and_failure() {
	section("cancel and failure");

	download_manager m;
	auto *tor = new fake_torrent_source;
	m.add_source(tor);
	m.set_consent("torrent", true);

	QString err;
	QList<int> ids;
	for (int i = 0; i < 5; ++i)
		ids << m.enqueue(QUrl(QString("magnet:?xt=urn:btih:%1").arg(i)), QString(), &err);

	// Cancel one that is running.
	m.cancel(ids[0]);
	check(job_by_id(m, ids[0])->status == download_state::cancelled,
	      "a running job cancels");
	check(!tor->live(ids[0]), "and the source is told");
	check(job_by_id(m, ids[0])->error.isEmpty(),
	      "a cancelled job is not relabelled as failed");

	// Cancel one that is still queued -- the source never knew about it.
	const int queued_id = ids[4];
	check(!tor->live(queued_id), "precondition: still queued");
	m.cancel(queued_id);
	check(job_by_id(m, queued_id)->status == download_state::cancelled,
	      "a queued job cancels without involving the source");

	// A source that reports success *after* a cancel must not undo it. Sources
	// that stop cleanly do exactly this: they report what they managed.
	const int late = ids[2];
	m.cancel(late);
	check(status_of(m, late) == download_state::cancelled, "cancelled first");
	tor->stop_seeding(late);          // finished(ok = true), after the cancel
	check(status_of(m, late) == download_state::cancelled,
	      QString("and a later success does not rewrite it as done (%1)")
	          .arg(int(status_of(m, late))));

	// Failure carries its message.
	tor->fail(ids[1], "Tracker refused.");
	const download_job *j = job_by_id(m, ids[1]);
	check(j->status == download_state::failed, "a failed job is marked failed");
	check(j->error == "Tracker refused.", "and keeps the source's message");

	// start() returning false is a failure too, and must not wedge the queue.
	download_manager m2;
	auto *tor2 = new fake_torrent_source;
	tor2->fail_on_start = true;
	m2.add_source(tor2);
	m2.set_consent("torrent", true);
	const int bad = m2.enqueue(QUrl("magnet:?xt=urn:btih:zzz"), QString(), &err);
	check(job_by_id(m2, bad)->status == download_state::failed,
	      "a source refusing at start() fails the job");
	check(job_by_id(m2, bad)->error == "Refused on purpose.",
	      "with its reason");

	tor2->fail_on_start = false;
	const int good = m2.enqueue(QUrl("magnet:?xt=urn:btih:yyy"), QString(), &err);
	check(tor2->live(good), "and the queue still runs afterwards");
}

// Makes a one-second file with ffmpeg, or returns false. Bounded: the
// inputs are generated and one second long, and the wait is capped.
static bool make_media(const QStringList &args) {
	QProcess p;
	p.start(QStandardPaths::findExecutable("ffmpeg"),
	        QStringList{ "-y", "-loglevel", "error" } + args);
	if (!p.waitForFinished(20000)) {
		p.kill();
		p.waitForFinished(2000);
		return false;
	}
	return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// **A picture and its sound, saved as one.** YouTube serves almost every
// quality as one or the other, and a Download of a picture-only row saved a
// silent file.
static void test_pairs(const QString &dir) {
	section("pairs: a picture and its sound become one file");

	// **Every Download on a yt-dlp row was disabled**: the HTTP source judges
	// an address by its shape, and YouTube's do not look like files.
	{
		http_download_source http;
		const QUrl yt("https://rr1---sn-x.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4");
		QString why;
		check(!http.accepts(yt, &why), "by its shape a stream address is refused");
		check(http.accepts_known_media(yt, &why),
		      "but taken when yt-dlp has said it is a media file");
		check(!http.accepts_known_media(QUrl("https://cdn.test/a/index.m3u8"), &why),
		      "while a manifest is still refused, whoever vouches for it");
		download_manager m;
		m.add_source(new http_download_source);
		check(m.enqueue(yt, QString(), &why) == 0, "the manager refuses it unvouched");
		m.set_directory(dir + "/vouched");
		const int id = m.enqueue(yt, QString(), &why, {}, "x.m4a",
		                         download_hints{ true, 0 });
		check(id != 0, QString("and takes it vouched (%1)").arg(why));
		m.cancel(id);
	}

	using names = download_manager::pair_names;
	const names mp4 = download_manager::names_for_pair("A_-_B_[id].mp4",
	                                                    "A_-_B_[id].m4a");
	check(mp4.picture == "A_-_B_[id].picture.mp4" &&
	      mp4.sound == "A_-_B_[id].sound.m4a" && mp4.combined == "A_-_B_[id].mp4",
	      QString("H.264 with AAC stays MP4, each half marked (%1, %2, %3)")
	          .arg(mp4.picture, mp4.sound, mp4.combined));
	check(download_manager::names_for_pair("x.webm", "x.webm").combined == "x.webm",
	      "VP9 with Opus stays WebM");
	check(download_manager::names_for_pair("x.webm", "x.m4a").combined == "x.mkv",
	      "and a mixed pair goes to Matroska, which takes both");

	QString err;
	{
		download_manager m;
		auto *src = new fake_plain_source;
		m.add_source(src);
		const int pic = m.enqueue_pair(QUrl("http://cdn.test/v"), QUrl("http://cdn.test/a"),
		                               QString(), &err, {}, "x.picture.mp4",
		                               "x.sound.m4a", "x.mp4");
		const download_job *pj = job_by_id(m, pic);
		check(pic && pj && pj->sound_job, QString("a pair is two jobs (%1)").arg(err));
		const int snd = pj ? pj->sound_job : 0;
		m.cancel(pic);
		check(status_of(m, snd) == download_state::cancelled,
		      "cancelling the picture cancels its sound");
	}
	{
		download_manager m;
		auto *src = new fake_plain_source;
		m.add_source(src);
		const int pic = m.enqueue_pair(QUrl("http://cdn.test/v"), QUrl("http://cdn.test/a"),
		                               QString(), &err, {}, "x.picture.mp4",
		                               "x.sound.m4a", "x.mp4");
		const int snd = job_by_id(m, pic)->sound_job;
		src->set(pic, download_state::running, 10, 10, dir + "/x.picture.mp4", QString());
		src->finish(pic, true, QString());
		check(status_of(m, pic) == download_state::running,
		      "a picture that lands first waits for its sound");
		src->finish(snd, false, "403 Forbidden");
		const download_job *pj = job_by_id(m, pic);
		check(pj && pj->status == download_state::done &&
		      pj->detail.contains("without its sound") && pj->detail.contains("403"),
		      QString("a failed sound leaves the picture, saying so (%1)")
		          .arg(pj ? pj->detail : QString()));
	}

	if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
		std::printf("  skip  combining needs ffmpeg, which is not installed\n");
		return;
	}
	const QString v = dir + "/y.picture.mp4", a = dir + "/y.sound.m4a";
	const bool made =
	  make_media({ "-f", "lavfi", "-i", "testsrc=duration=1:size=64x64:rate=5",
	               "-c:v", "mpeg4", v }) &&
	  make_media({ "-f", "lavfi", "-i", "sine=duration=1", "-c:a", "aac", a });
	check(made, "ffmpeg made a one-second picture and sound to combine");
	if (!made)
		return;

	download_manager m;
	auto *src = new fake_plain_source;
	m.add_source(src);
	const int pic = m.enqueue_pair(QUrl("http://cdn.test/v"), QUrl("http://cdn.test/a"),
	                               QString(), &err, {}, "y.picture.mp4",
	                               "y.sound.m4a", "y.mp4");
	const int snd = job_by_id(m, pic)->sound_job;
	src->set(snd, download_state::running, 1, 1, a, QString());
	src->set(pic, download_state::running, 1, 1, v, QString());
	src->finish(snd, true, QString());
	src->finish(pic, true, QString());
	QElapsedTimer t;
	t.start();
	while (status_of(m, pic) != download_state::done && t.elapsed() < 20000)
		spin(50);
	const download_job *pj = job_by_id(m, pic);
	const QString out = dir + "/y.mp4";
	check(pj && pj->status == download_state::done && pj->path == out,
	      QString("the picture's row ends as the combined file (%1: %2)")
	          .arg(pj ? pj->path : QString(), pj ? pj->detail : QString()));
	check(QFileInfo(out).size() > 0, "which exists and has bytes");
	check(!QFile::exists(v) && !QFile::exists(a), "the two halves are removed");
	check(!job_by_id(m, snd), "and the sound's row, whose file is gone, with them");

	QProcess probe;
	probe.start(QStandardPaths::findExecutable("ffmpeg"), { "-hide_banner", "-i", out });
	probe.waitForFinished(10000);
	const QString said = QString::fromLocal8Bit(probe.readAllStandardError());
	check(said.contains("Video:") && said.contains("Audio:"),
	      "and it holds a picture and a sound");
}

static void test_reentrancy() {
	section("re-entrancy: a source that fails inside start()");

	download_manager m;
	m.add_source(new exploding_source);

	QString err;
	const int a = m.enqueue(QUrl("boom://one"), QString(), &err);
	const int b = m.enqueue(QUrl("boom://two"), QString(), &err);

	check(job_by_id(m, a)->status == download_state::failed, "the first fails");
	check(job_by_id(m, b)->status == download_state::failed, "the second fails");
	check(job_by_id(m, a)->error.contains("Detonated"), "with the right message");
	check(m.jobs().size() == 2, "and no job is lost or duplicated");
}

static void test_pause() {
	section("pause: routed only where the source says it is possible");

	download_manager m;
	auto *tor = new fake_torrent_source;
	m.add_source(tor);
	m.set_consent("torrent", true);

	QString err;
	const int id = m.enqueue(QUrl("magnet:?xt=urn:btih:abc"), QString(), &err);
	m.pause(id);
	check(tor->pause_calls == 1, "pause reaches a resumable source");
	check(job_by_id(m, id)->status == download_state::paused, "and is reflected");
	m.unpause(id);
	check(tor->unpause_calls == 1, "unpause too");
	check(job_by_id(m, id)->status == download_state::running, "back to running");
}


// ------------------------------------------------- a server that says no --
// Range is a request, not a command: a server may answer 200 with the whole
// body instead, and that is correct HTTP. The download source used to ask for
// a range, open the file in Append, and write whatever came back -- so against
// a server like this one a resumed download appended the complete body to the
// bytes already on disk and produced a file of exactly twice the right size,
// reported as done.
//
// The helper this suite used always answered 206, so the case never arose. This
// one is in-process and unconditional, because the bug was found on a phone
// against python's http.server and should not have needed a phone.
class no_range_server : public QTcpServer {
public:
	static const int k_size = 40000;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			// Deliberately ignoring any Range header, and saying 200.
			const QByteArray body(k_size, 'z');
			s->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
			          "Content-Length: " + QByteArray::number(body.size()) +
			          "\r\nConnection: close\r\n\r\n" + body);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// Honours `Range: bytes=a-b` with 206 and Content-Range, as googlevideo
// does, and counts what it was asked. `starve_after` answers every request
// past that many with a 206 and no bytes.
class range_server : public QTcpServer {
public:
	static const int k_size = 35000;
	QList<QByteArray> asked;
	int starve_after = -1;
	// Sends every request for /videoplayback on to /real first, as YouTube
	// sends each to another host.
	bool redirect = false;

	static QByteArray body() {
		QByteArray b(k_size, 0);
		for (int i = 0; i < k_size; ++i)
			b[i] = char(i % 251);
		return b;
	}

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			if (redirect && head.startsWith("GET /videoplayback")) {
				s->write("HTTP/1.1 302 Found\r\nLocation: /real\r\n"
				          "Content-Length: 0\r\nConnection: close\r\n\r\n");
				s->flush();
				s->disconnectFromHost();
				return;
			}
			QByteArray range;
			for (const QByteArray &line : head.split('\n'))
				if (line.toLower().startsWith("range:"))
					range = line.mid(6).trimmed();
			asked << range;
			qint64 from = 0, to = k_size - 1;
			if (range.startsWith("bytes=")) {
				const QList<QByteArray> ab = range.mid(6).split('-');
				from = ab.value(0).toLongLong();
				if (!ab.value(1).isEmpty())
					to = qMin<qint64>(ab.value(1).toLongLong(), k_size - 1);
			}
			const bool starve = starve_after >= 0 && asked.size() > starve_after;
			const QByteArray part = starve ? QByteArray()
			                               : body().mid(from, to - from + 1);
			s->write("HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\n"
			          "Content-Range: bytes " + QByteArray::number(from) + '-' +
			          QByteArray::number(to) + '/' + QByteArray::number(k_size) +
			          "\r\nContent-Length: " + QByteArray::number(part.size()) +
			          "\r\nConnection: close\r\n\r\n" + part);
			s->flush();
			s->disconnectFromHost();
		});
	}
};

// **YouTube throttles one open-ended request to about 32 KB/s** and answers
// a 10 MB range in a second, so yt-dlp fetches it a range at a time and
// says how large: measured 2026-10-11, 1.9 MB in 60 s against 10 MB in 1.1 s.
static void test_chunked(const QString &tmp) {
	section("http source: a stream fetched a range at a time");

	auto run = [&](range_server &server, const QString &name, qint64 chunk) {
		download_manager m;
		m.add_source(new http_download_source);
		m.set_directory(tmp);
		QString err;
		const int id = m.enqueue(
		    QUrl(QString("http://127.0.0.1:%1/videoplayback?itag=1")
		             .arg(server.serverPort())),
		    QString(), &err, {}, name, download_hints{ true, chunk });
		QElapsedTimer t;
		t.start();
		while (job_by_id(m, id) && !job_by_id(m, id)->terminal() && t.elapsed() < 8000)
			spin(20);
		const download_job *j = job_by_id(m, id);
		return j ? *j : download_job{};
	};

	range_server server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("  --    could not listen; skipped\n");
		return;
	}
	const download_job j = run(server, "whole.mp4", 10000);
	check(j.status == download_state::done, QString("the job completes (%1)").arg(j.error));
	const QList<QByteArray> four = { "bytes=0-9999", "bytes=10000-19999",
	                                 "bytes=20000-29999", "bytes=30000-39999" };
	check(server.asked == four,
	      QString("in four ranges of the size asked (%1)")
	          .arg(QString::fromLatin1(server.asked.join(", "))));
	QFile f(tmp + "/whole.mp4");
	f.open(QIODevice::ReadOnly);
	check(f.readAll() == range_server::body(), "and the file is the body, byte for byte");
	check(j.total == range_server::k_size,
	      QString("its size is the whole, from Content-Range (%1)").arg(j.total));

	// **Through a redirect, as every YouTube stream is.** The status read at
	// the hop was 302, so the first version stopped after one range.
	range_server hop;
	hop.redirect = true;
	if (!hop.listen(QHostAddress::LocalHost, 0))
		return;
	const download_job h = run(hop, "hop.mp4", 10000);
	QFile hf(tmp + "/hop.mp4");
	hf.open(QIODevice::ReadOnly);
	check(h.status == download_state::done && hf.readAll() == range_server::body(),
	      QString("every range through a redirect, whole (%1 ranges, %2)")
	          .arg(hop.asked.size()).arg(h.error));

	// And a resume through one: the hop's status emptied the partial file.
	{
		QFile part(tmp + "/resumed.mp4");
		part.open(QIODevice::WriteOnly);
		part.write(range_server::body().left(12345));
	}
	range_server hop2;
	hop2.redirect = true;
	if (!hop2.listen(QHostAddress::LocalHost, 0))
		return;
	const download_job r = run(hop2, "resumed.mp4", 0);
	QFile rf(tmp + "/resumed.mp4");
	rf.open(QIODevice::ReadOnly);
	check(r.status == download_state::done && rf.readAll() == range_server::body() &&
	      hop2.asked == QList<QByteArray>({ "bytes=12345-" }),
	      QString("a resume through a redirect keeps what it had (%1, %2)")
	          .arg(QString::fromLatin1(hop2.asked.join(", ")), r.error));

	range_server starved;
	starved.starve_after = 2;
	if (!starved.listen(QHostAddress::LocalHost, 0))
		return;
	const download_job s = run(starved, "starved.mp4", 10000);
	check(s.status == download_state::failed && s.error.contains("stopped sending"),
	      QString("a server that stops sending fails the job rather than being "
	               "asked for ever (%1)").arg(s.error));
	check(starved.asked.size() == 3, QString("after one empty answer (%1 asked)")
	                                     .arg(starved.asked.size()));
}

static void test_resume_against_a_server_that_ignores_range(const QString &tmp) {
	section("http source: a server that ignores Range does not corrupt the file");

	no_range_server server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("  --    could not listen; skipped\n");
		return;
	}
	const QString url = QString("http://127.0.0.1:%1/clip.mp4").arg(server.serverPort());

	// A file already on disk, as a half-finished download would leave.
	//
	// **Checked, because a fixture that was not written is not a fixture.**
	// Both calls returned a status nobody read, so a directory this process
	// could not write produced a zero-length file and the section then failed
	// on "the job completes" -- which reads as the downloader being broken.
	// A fixture that cannot be built is reported as that and nothing else is
	// claimed.
	QFile::remove(tmp + "/clip.mp4");
	{
		QFile f(tmp + "/clip.mp4");
		if (!f.open(QIODevice::WriteOnly)) {
			check(false, QString("could not create the half-finished file in "
			                      "%1 (%2), so nothing below was measured")
			                 .arg(tmp, f.errorString()));
			return;
		}
		if (f.write(QByteArray(12345, 'x')) != 12345) {
			check(false, "could not write the half-finished file, so nothing "
			              "below was measured");
			return;
		}
	}

	download_manager m;
	m.add_source(new http_download_source);
	m.set_directory(tmp);
	QString err;
	const int id = m.enqueue(QUrl(url), QString(), &err);
	QElapsedTimer t;
	t.start();
	while (job_by_id(m, id) && !job_by_id(m, id)->terminal() && t.elapsed() < 8000)
		spin(20);

	const download_job *j = job_by_id(m, id);
	check(j && j->status == download_state::done, "the job completes");
	const qint64 on_disk = QFileInfo(tmp + "/clip.mp4").size();
	check(on_disk == no_range_server::k_size,
	      QString("and the file is the size the server sent, not that plus what "
	               "was already there (%1, wanted %2)")
	          .arg(on_disk)
	          .arg(no_range_server::k_size));
	// The bytes matter as much as the count: appending would leave the old 'x'
	// bytes at the front and still be the wrong file even at the right length.
	QFile check_file(tmp + "/clip.mp4");
	check_file.open(QIODevice::ReadOnly);
	const QByteArray head = check_file.read(16);
	// Sixteen bytes or the question was not asked: a read that returned
	// nothing contains no stale 'x' either, and would report the file correct
	// precisely when it could not be examined.
	check(head.size() == 16,
	      QString("there are bytes to inspect (%1)").arg(head.size()));
	check(!head.contains('x'),
	      "and it is the server's bytes from the first one, not the stale ones");
}

// ------------------------------------------------------------------- http --
// The transport that moved behind the seam must behave exactly as before.
static void test_http_against_server(const QString &base, const QString &tmp) {
	section("http source: unchanged behaviour through the seam");

	download_manager m;
	m.add_source(new http_download_source);
	m.set_directory(tmp);

	QString err;
	const int id = m.enqueue(QUrl(base + "/payload.mp4"), "node-3", &err);
	check(id != 0, "a direct file is accepted");

	QElapsedTimer t;
	t.start();
	while (!job_by_id(m, id)->terminal() && t.elapsed() < 8000)
		spin(20);

	const download_job *j = job_by_id(m, id);
	check(j->status == download_state::done, "it completes");
	check(j->received == 65536, "with the whole file");
	check(j->total == 65536, "and a known total");
	check(QFileInfo(j->path).size() == 65536, "and the bytes are on disk");
	check(j->node_id == "node-3", "and it remembers the node it came from");
	check(j->source_id == "http", "attributed to the http source");

	// Resume: leave a partial file and download again.
	section("http source: resume via Range still works");
	QFile::remove(tmp + "/partial.mp4");
	{
		QFile f(tmp + "/partial.mp4");
		f.open(QIODevice::WriteOnly);
		f.write(QByteArray(30000, 'x'));
		f.close();
	}
	const int rid = m.enqueue(QUrl(base + "/partial.mp4"), QString(), &err);
	t.restart();
	while (!job_by_id(m, rid)->terminal() && t.elapsed() < 8000)
		spin(20);
	const download_job *rj = job_by_id(m, rid);
	check(rj->status == download_state::done, "the resumed job completes");
	check(QFileInfo(rj->path).size() == 65536,
	      "and the file ends up the right size, not appended twice");
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	test_routing();
	test_consent();
	test_torrent_lifecycle();
	test_concurrency();
	test_independent_sources();
	test_cancel_and_failure();
	test_reentrancy();
	test_pause();

	// **Its own directory rather than the shared temp root.** This passed
	// `QDir::tempPath()` and wrote `clip.mp4` straight into it -- no prefix,
	// no uniqueness -- so two runs collide, and a run by a different user
	// from the one who last left that file there cannot open it at all. The
	// Makefile sets TMPDIR for exactly this reason, which protects
	// `make test` and not the person running a driver directly, who is the
	// person whose binary is already misbehaving.
	//
	// Measured: run straight from `build-make`, the section reported "the job
	// completes" red with `QIODevice::write ... device not open` above it --
	// a permission on a file from another user, wearing the costume of a
	// broken downloader.
	QTemporaryDir scratch;
	if (scratch.isValid()) {
		test_resume_against_a_server_that_ignores_range(scratch.path());
		test_pairs(scratch.path());
		test_chunked(scratch.path());
	}
	else
		std::printf("  --    no scratch directory to run the resume case in; "
		             "skipped\n");

	if (argc > 2)
		test_http_against_server(QString::fromLocal8Bit(argv[1]),
		                          QString::fromLocal8Bit(argv[2]));
	else
		std::printf("\n(skipping http tests: no server URL given)\n");

	// **Pause was offered on every direct download and did nothing.** The
	// manager and the downloads dialog both gated it on `resumable`, whose
	// own comment says it is about surviving a *restart* -- and
	// `http_download_source` is resumable and overrides neither `pause` nor
	// `unpause`. So the button was enabled, pressing it called the base
	// class's empty body, the bytes kept arriving, the job never entered
	// `paused`, and Resume could never become reachable either.
	//
	// One flag was answering two questions. `pausable` is the second one, and
	// nothing infers it from the first, because inferring it from a
	// neighbouring capability is exactly what went wrong.
	section("pause is offered only where it does something");
	{
		download_manager m;
		auto *plain = new fake_unpausable_source;   // resumable, not pausable
		auto *rich  = new fake_plain_source;        // both
		m.add_source(plain);
		m.add_source(rich);

		check(plain->capabilities().resumable && !plain->capabilities().pausable,
		      "a source can be resumable and not pausable");
		check(rich->capabilities().pausable,
		      "while one that overrides pause says so");

		// **The manager refuses rather than calling and watching nothing
		// happen**, which is what the seam's own comment asks for.
		QString err;
		const int id = m.enqueue(QUrl("http://example.invalid/big.bin"),
		                          QString(), &err);
		check(id > 0, QString("a job is queued (%1, %2)").arg(id).arg(err));
		spin(80);

		auto status_of = [&](int want) {
			for (const download_job &j : m.jobs())
				if (j.id == want)
					return int(j.status);
			return -1;
		};
		auto source_of = [&](int want) {
			for (const download_job &j : m.jobs())
				if (j.id == want)
					return j.source_id;
			return QString();
		};
		check(source_of(id) == plain->id(),
		      QString("it landed on the source that cannot pause (%1)")
		          .arg(source_of(id)));

		// **The manager must not call it at all**, which is what the seam's
		// own comment asks for: it learns from the capability "rather than by
		// calling and watching nothing happen".
		//
		// Asserting the job's status instead would prove nothing -- the base
		// class's `pause` is empty, so gating on the wrong flag and gating on
		// the right one leave the status equally unchanged. The call count is
		// the only thing that separates them.
		m.pause(id);
		m.unpause(id);
		spin(80);
		check(plain->pause_calls == 0 && plain->unpause_calls == 0,
		      QString("the manager does not call a source that says it cannot "
		               "pause (%1 pause, %2 unpause)")
		          .arg(plain->pause_calls).arg(plain->unpause_calls));
		check(status_of(id) != int(download_state::paused),
		      QString("and the job is not left claiming to be paused (%1)")
		          .arg(status_of(id)));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
