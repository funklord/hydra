// Does the subprocess plumbing actually work end to end? Tolerant by design:
// this depends on the network, and a failure here is not a code failure.
#include "ytdlp_resolver.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString url = argc > 1 ? argv[1] : "";
	if (url.isEmpty()) { std::printf("no url given\n"); return 0; }

	ytdlp_resolver r;
	std::printf("using: %s\n", qPrintable(r.description()));
	if (!r.available()) { std::printf("SKIP: no yt-dlp\n"); return 0; }

	QEventLoop loop;
	QObject::connect(&r, &ytdlp_resolver::resolved, [&](const resolved_media &m) {
		std::printf("RESOLVED extractor=%s title=%s formats=%d\n",
		             qPrintable(m.extractor), qPrintable(m.title.left(50)),
		             int(m.formats.size()));
		const media_format b = ytdlp_resolver::best(m);
		std::printf("BEST id=%s proto=%s ext=%s height=%d size=%lld\n",
		             qPrintable(b.format_id), qPrintable(b.protocol),
		             qPrintable(b.ext), b.height, b.filesize);
		std::printf("URL %s\n", qPrintable(b.url.toString().left(110)));
		std::printf("HEADERS %d\n", int(b.headers.size()));
		loop.quit();
	});
	QObject::connect(&r, &ytdlp_resolver::failed, [&](const QString &e) {
		std::printf("FAILED %s\n", qPrintable(e.left(160)));
		loop.quit();
	});
	QTimer::singleShot(75000, [&] { std::printf("TIMEOUT\n"); loop.quit(); });
	r.resolve(QUrl(url));
	loop.exec();
	std::printf("done\n");
	return 0;
}
