// SPDX-License-Identifier: GPL-3.0-or-later
#include "ytdlp_resolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

namespace {

// Where a vendored checkout might be, relative to the running binary. A build
// tree puts the executable in build/, an install puts it in bin/; both are
// covered, and HYDRA_YTDLP overrides everything for anyone who keeps it
// elsewhere.
QStringList vendored_candidates() {
	const QString app = QCoreApplication::applicationDirPath();
	QStringList out;
	const QByteArray env = qgetenv("HYDRA_YTDLP");
	if (!env.isEmpty())
		out << QString::fromLocal8Bit(env);
	// `const char *`, not `const QString &`: an initializer list of string
	// literals makes a temporary QString per iteration, and binding a reference
	// to it is the kind of thing that is safe here and is not safe one refactor
	// later. The conversion happens at the call below either way.
	for (const char *rel : { "third_party/yt-dlp", "../third_party/yt-dlp",
		                        "../../third_party/yt-dlp" })
		out << QDir(app).absoluteFilePath(QLatin1String(rel));
	out << QDir::current().absoluteFilePath("third_party/yt-dlp");
	return out;
}

bool looks_like_checkout(const QString &dir) {
	return QFileInfo::exists(QDir(dir).filePath("yt_dlp/__main__.py"));
}

}  // namespace

ytdlp_resolver::ytdlp_resolver(QObject *parent) : QObject(parent) {
	refresh();
}

ytdlp_resolver::~ytdlp_resolver() { cancel(); }

void ytdlp_resolver::refresh() {
	m_program.clear();
	m_prefix.clear();
	m_origin.clear();

	// **The vendored copy first, because it is pinned.** This used to prefer
	// PATH, on the reasoning that the package manager keeps that one current
	// and staying current is most of what yt-dlp is for. The reasoning was
	// sound and the premise is not: measured on this machine, PATH held
	// 2025.04.30 and the submodule 2026.07.04, fourteen months the other way,
	// and a stable distribution is where that gap is widest rather than an
	// accident. Preferring PATH bought the older extractors.
	//
	// Determinism is the better reason to keep it this way round even where
	// PATH happens to be newer. What this resolver returns is handed to the
	// model as worked reference and used as ground truth, so the same page
	// resolving differently on two machines makes that pipeline's input
	// depend on whatever a distribution shipped. The pin makes the extractor
	// set a fact of the tree, and bumping the submodule is then the one
	// reviewable act that changes it.
	//
	// python3 is what makes the checkout runnable; without an interpreter it
	// is inert source, so fall through to PATH rather than reporting nothing.
	const QString python = QStandardPaths::findExecutable("python3");
	if (!python.isEmpty()) {
		for (const QString &dir : vendored_candidates()) {
			if (!looks_like_checkout(dir))
				continue;
			m_program = python;
			m_prefix  = { "-m", "yt_dlp" };
			m_origin  = dir;
			return;
		}
	}

	// Else whatever the machine has, which is what a tree cloned without
	// `--recurse-submodules` falls back to -- easy to do, and the README
	// warns about it.
	const QString on_path = QStandardPaths::findExecutable("yt-dlp");
	if (!on_path.isEmpty()) {
		m_program = on_path;
		m_origin  = "PATH";
	}
}

QString ytdlp_resolver::description() const {
	if (m_program.isEmpty())
		return "yt-dlp not found — clone with --recurse-submodules, or install it";
	if (m_origin == "PATH")
		return QString("yt-dlp from PATH (%1)").arg(m_program);
	return QString("vendored yt-dlp (%1)").arg(m_origin);
}

bool ytdlp_resolver::busy() const {
	return m_proc && m_proc->state() != QProcess::NotRunning;
}

void ytdlp_resolver::cancel() {
	if (!m_proc)
		return;
	m_proc->disconnect(this);
	if (m_proc->state() != QProcess::NotRunning) {
		m_proc->terminate();
		if (!m_proc->waitForFinished(1500))
			m_proc->kill();
	}
	m_proc->deleteLater();
	m_proc = nullptr;
}

void ytdlp_resolver::resolve(const QUrl &page_url) {
	if (!available()) {
		emit failed("yt-dlp is not available.");
		return;
	}
	cancel();

	auto *proc = new QProcess(this);
	m_proc = proc;
	if (!m_origin.isEmpty() && m_origin != "PATH")
		proc->setWorkingDirectory(m_origin);

	QStringList args = m_prefix;
	args << "--dump-single-json"
	     << "--no-warnings"
	     << "--no-playlist"        // a watch page's "playlist" is usually related items
	     << "--no-progress"
	     << "--socket-timeout" << "20"
	     << page_url.toString();
	proc->setProgram(m_program);
	proc->setArguments(args);

	connect(proc, &QProcess::finished, this,
	         [this, proc](int code, QProcess::ExitStatus) {
		const QByteArray out = proc->readAllStandardOutput();
		const QByteArray err = proc->readAllStandardError();
		proc->deleteLater();
		if (m_proc == proc)
			m_proc = nullptr;

		if (code != 0 || out.isEmpty()) {
			// yt-dlp's own message is far more useful than anything invented
			// here -- "Unsupported URL" is the answer that matters, and it is
			// the answer the long-tail paths (sec 11.5, sec 11.6) exist for.
			QString reason = QString::fromUtf8(err).trimmed();
			if (reason.isEmpty())
				reason = QString("yt-dlp exited with %1").arg(code);
			emit failed(reason.section('\n', 0, 0).left(300));
			return;
		}
		const resolved_media m = parse(out);
		if (!m.ok) {
			emit failed(m.error.isEmpty() ? "Could not read yt-dlp's answer."
			                              : m.error);
			return;
		}
		emit resolved(m);
	});
	connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError) {
		const QString msg = proc->errorString();
		proc->deleteLater();
		if (m_proc == proc)
			m_proc = nullptr;
		emit failed("Could not run yt-dlp: " + msg);
	});

	proc->start();
}

// ---------------------------------------------------------------------------

resolved_media ytdlp_resolver::parse(const QByteArray &json) {
	resolved_media out;
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (doc.isNull() || !doc.isObject()) {
		out.error = "yt-dlp did not return JSON: " + err.errorString();
		return out;
	}
	QJsonObject o = doc.object();

	// A page that yields a playlist is nearly always one video plus related
	// items, so take the first entry rather than refusing.
	if (o.value("_type").toString() == "playlist") {
		const QJsonArray entries = o.value("entries").toArray();
		if (entries.isEmpty()) {
			out.error = "yt-dlp found a playlist with nothing in it.";
			return out;
		}
		o = entries.first().toObject();
	}

	out.title       = o.value("title").toString();
	out.extractor   = o.value("extractor_key").toString();
	if (out.extractor.isEmpty())
		out.extractor = o.value("extractor").toString();
	out.webpage_url = QUrl(o.value("webpage_url").toString());

	const QJsonArray formats = o.value("formats").toArray();
	auto read_format = [](const QJsonObject &f) {
		media_format m;
		m.format_id = f.value("format_id").toString();
		m.url       = QUrl(f.value("url").toString());
		m.ext       = f.value("ext").toString();
		m.protocol  = f.value("protocol").toString();
		m.note      = f.value("format_note").toString();
		m.height    = f.value("height").toInt();
		// filesize is often absent and filesize_approx present; either is
		// better than claiming not to know.
		if (f.value("filesize").isDouble())
			m.filesize = qint64(f.value("filesize").toDouble());
		else if (f.value("filesize_approx").isDouble())
			m.filesize = qint64(f.value("filesize_approx").toDouble());
		// "none" is yt-dlp's way of saying this rendition has no such stream.
		m.has_video = f.value("vcodec").toString() != "none";
		m.has_audio = f.value("acodec").toString() != "none";
		const QJsonObject hs = f.value("http_headers").toObject();
		for (auto it = hs.begin(); it != hs.end(); ++it)
			m.headers.insert(it.key(), it.value().toString());
		return m;
	};

	for (const QJsonValue &v : formats) {
		const media_format m = read_format(v.toObject());
		if (m.url.isValid() && !m.url.isEmpty())
			out.formats << m;
	}
	// Some extractors report a single URL and no formats array at all.
	if (out.formats.isEmpty() && !o.value("url").toString().isEmpty())
		out.formats << read_format(o);

	if (out.formats.isEmpty()) {
		out.error = "yt-dlp reported no playable formats.";
		return out;
	}
	out.ok = true;
	return out;
}

media_format ytdlp_resolver::best(const resolved_media &m) {
	auto progressive = [](const media_format &f) {
		return f.has_video && f.has_audio &&
		       (f.protocol == "https" || f.protocol == "http");
	};

	// Progressive first, tallest of those. It needs no assembly, resumes with a
	// Range request and plays in anything -- worth more than extra pixels behind
	// a manifest this build cannot yet assemble for every player.
	media_format pick;
	for (const media_format &f : m.formats) {
		if (!progressive(f))
			continue;
		if (pick.url.isEmpty() || f.height > pick.height)
			pick = f;
	}
	if (!pick.url.isEmpty())
		return pick;

	// Otherwise the tallest rendition that at least carries both streams.
	for (const media_format &f : m.formats) {
		if (!f.has_video || !f.has_audio)
			continue;
		if (pick.url.isEmpty() || f.height > pick.height)
			pick = f;
	}
	if (!pick.url.isEmpty())
		return pick;

	return m.formats.isEmpty() ? media_format{} : m.formats.last();
}
