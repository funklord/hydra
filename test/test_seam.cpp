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
#include <QSignalSpy>
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
	err.clear();
	m.enqueue(QUrl("https://e.example/stream.m3u8"), QString(), &err);
	check(err.contains("Watch in player"),
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

static void test_resume_against_a_server_that_ignores_range(const QString &tmp) {
	section("http source: a server that ignores Range does not corrupt the file");

	no_range_server server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("  --    could not listen; skipped\n");
		return;
	}
	const QString url = QString("http://127.0.0.1:%1/clip.mp4").arg(server.serverPort());

	// A file already on disk, as a half-finished download would leave.
	QFile::remove(tmp + "/clip.mp4");
	{
		QFile f(tmp + "/clip.mp4");
		f.open(QIODevice::WriteOnly);
		f.write(QByteArray(12345, 'x'));
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

	test_resume_against_a_server_that_ignores_range(QDir::tempPath());

	if (argc > 2)
		test_http_against_server(QString::fromLocal8Bit(argv[1]),
		                          QString::fromLocal8Bit(argv[2]));
	else
		std::printf("\n(skipping http tests: no server URL given)\n");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
