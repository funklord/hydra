// Downloading a learned stream: does it send what the CDN asked for, and does
// resume still own the Range header?
#include "download_manager.h"
#include "http_download_source.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

static const download_job *job_of(const download_manager &m, int id) {
	for (const download_job &j : m.jobs()) if (j.id == id) return &j;
	return nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString base = argc > 1 ? argv[1] : "http://127.0.0.1:8851";
	const QString dir  = QDir::temp().filePath("hydra-dlhdr");
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);

	download_manager m;
	m.add_source(new http_download_source);
	m.set_directory(dir);

	section("a download carries the headers it was given");
	{
		QMap<QString, QString> h;
		h.insert("Referer", "https://site.example/watch/1");
		h.insert("User-Agent", "Hydra/1.0 dl");
		h.insert("X-Token", "abc123");

		QString err;
		const int id = m.enqueue(QUrl(base + "/stream.mp4"), "n1", &err, h);
		check(id != 0, QString("accepted (%1)").arg(err));

		QElapsedTimer t; t.start();
		while (!job_of(m, id)->terminal() && t.elapsed() < 15000) spin(50);
		check(job_of(m, id)->status == download_state::done, "it completes");

		QFile f(job_of(m, id)->path);
		f.open(QIODevice::ReadOnly);
		const QString seen = QString::fromUtf8(f.readAll());
		check(seen.contains("https://site.example/watch/1"),
		      "the Referer reached the server");
		check(seen.contains("Hydra/1.0 dl"), "and the User-Agent");
		check(seen.contains("abc123"), "and a header the extractor invented");
	}

	section("resume still owns Range");
	{
		// A partial file on disk means the source must ask for the rest — and a
		// caller naming Range itself must not be able to move that offset.
		const QString path = QDir(dir).filePath("resume.mp4");
		{ QFile f(path); f.open(QIODevice::WriteOnly); f.write(QByteArray(500, 'x')); }

		QMap<QString, QString> h;
		h.insert("Referer", "https://site.example/2");
		h.insert("Range", "bytes=999999-");     // hostile or careless

		QString err;
		const int id = m.enqueue(QUrl(base + "/resume.mp4"), QString(), &err, h);
		QElapsedTimer t; t.start();
		while (!job_of(m, id)->terminal() && t.elapsed() < 15000) spin(50);

		QFile f(path);
		f.open(QIODevice::ReadOnly);
		const QString seen = QString::fromUtf8(f.readAll());
		check(seen.contains("bytes=500-"),
		      "the source's own resume offset is what was sent");
		check(!seen.contains("999999"),
		      "and the caller's Range was ignored, not merged");
		check(seen.contains("https://site.example/2"),
		      "while its other headers still went");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
