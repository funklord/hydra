#include "ytdlp_updater.h"

#include "ytdlp_resolver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace {

const char *k_repo = "https://github.com/yt-dlp/yt-dlp/releases";

// Bounds on what is read: the sums and signature are a few kilobytes and
// the release about three megabytes. Anything far larger is not what was
// asked for.
constexpr qint64 k_small_max   = 64 * 1024;
constexpr qint64 k_program_max = 64 * 1024 * 1024;

QSettings state() {
	return QSettings(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
}

}  // namespace

ytdlp_updater::ytdlp_updater(QObject *parent)
  : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString ytdlp_updater::directory() {
	return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
	    .filePath("yt-dlp");
}

QString ytdlp_updater::program() {
	const QString p = QDir(directory()).filePath("yt-dlp");
	return QFileInfo::exists(p) && !installed_version().isEmpty() ? p : QString();
}

QString ytdlp_updater::installed_version() {
	QFile f(QDir(directory()).filePath("version"));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll()).trimmed();
}

QString ytdlp_updater::python() {
	const QString py = QDir(directory()).filePath("venv/bin/python");
	return QFileInfo::exists(QDir(directory()).filePath("venv/impersonation-ok")) &&
	               QFileInfo(py).isExecutable()
	         ? py
	         : QString();
}

QString ytdlp_updater::tag_from(const QUrl &url) {
	static const QRegularExpression tag(QStringLiteral("/releases/tag/([^/?#]+)"));
	return tag.match(url.path()).captured(1);
}

QByteArray ytdlp_updater::sum_for(const QByteArray &sums, const QString &file) {
	// Lines of "<hex>  <name>", or "<hex> *<name>" for a binary-mode sum.
	for (const QByteArray &line : sums.split('\n')) {
		const QList<QByteArray> parts = line.simplified().split(' ');
		if (parts.size() != 2)
			continue;
		QByteArray name = parts[1];
		if (name.startsWith('*'))
			name.remove(0, 1);
		if (name == file.toUtf8() && parts[0].size() == 64)
			return parts[0].toLower();
	}
	return QByteArray();
}

QByteArray ytdlp_updater::dearmor(const QByteArray &armored) {
	// The base64 between the header block's blank line and the checksum
	// line ("=XXXX") or the END line.
	QByteArray body;
	bool in_block = false, past_headers = false;
	for (const QByteArray &raw : armored.split('\n')) {
		const QByteArray line = raw.trimmed();
		if (line.startsWith("-----BEGIN PGP PUBLIC KEY BLOCK")) {
			in_block = true;
			continue;
		}
		if (!in_block)
			continue;
		if (line.startsWith("-----END"))
			break;
		if (!past_headers) {
			if (line.isEmpty())
				past_headers = true;
			continue;
		}
		if (line.startsWith('='))
			break;
		body += line;
	}
	return QByteArray::fromBase64(body);
}

QByteArray ytdlp_updater::shipped_key() {
	for (const QString &dir : ytdlp_resolver::vendored_dirs()) {
		QFile f(QDir(dir).filePath("public.key"));
		if (f.open(QIODevice::ReadOnly))
			return f.readAll();
	}
	return QByteArray();
}

bool ytdlp_updater::verify(const QByteArray &data, const QByteArray &sig,
                           const QByteArray &armored, QString *why) {
	const QString gpgv = QStandardPaths::findExecutable("gpgv");
	if (gpgv.isEmpty()) {
		*why = "gpgv is not installed, so a release's signature cannot be checked";
		return false;
	}
	const QByteArray key = dearmor(armored);
	if (key.isEmpty()) {
		*why = "no yt-dlp release key to check the signature against";
		return false;
	}
	QTemporaryDir tmp;
	if (!tmp.isValid()) {
		*why = "no temporary directory to check the signature in";
		return false;
	}
	const auto put = [&tmp](const QString &name, const QByteArray &bytes) {
		QFile f(tmp.filePath(name));
		return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
	};
	if (!put("key.gpg", key) || !put("sums", data) || !put("sums.sig", sig)) {
		*why = "could not write the files to check";
		return false;
	}
	QProcess p;
	p.start(gpgv, { "--status-fd", "1", "--keyring", tmp.filePath("key.gpg"),
	                tmp.filePath("sums.sig"), tmp.filePath("sums") });
	if (!p.waitForFinished(30000)) {
		p.kill();
		p.waitForFinished(1000);
		*why = "gpgv did not answer";
		return false;
	}
	const QString status = QString::fromUtf8(p.readAllStandardOutput());
	// The exit status says the signature is good by some key in the
	// keyring; the status line says by which, and that is the one asked.
	const bool signer = status.contains(QStringLiteral("[GNUPG:] VALIDSIG %1 ")
	                                        .arg(QLatin1String(k_fingerprint)));
	if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0 || !signer) {
		*why = "the release's checksums are not signed by yt-dlp's release key";
		return false;
	}
	return true;
}

void ytdlp_updater::check(bool forced) {
	if (busy())
		return;
	QSettings s = state();
	const QDateTime last = s.value("ytdlp/checked").toDateTime();
	const QDateTime now  = QDateTime::currentDateTimeUtc();
	if (!forced && last.isValid() && last.secsTo(now) < 24 * 3600)
		return;
	// Recorded as it starts, finished or not, so a check that fails does not
	// run again on every lookup until the day is out.
	s.setValue("ytdlp/checked", now);
	m_tag.clear();
	m_sums.clear();
	m_want.clear();
	// Not followed: where `latest` points is the answer.
	get(QUrl(QString("%1/latest").arg(QLatin1String(k_repo))), false,
	    &ytdlp_updater::on_latest);
}

void ytdlp_updater::get(const QUrl &url, bool follow,
                        void (ytdlp_updater::*next)(QNetworkReply *)) {
	QNetworkRequest req(url);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                 follow ? QNetworkRequest::NoLessSafeRedirectPolicy
	                        : QNetworkRequest::ManualRedirectPolicy);
	req.setTransferTimeout(60000);
	m_reply = m_net->get(req);
	QNetworkReply *r = m_reply;
	const qint64 cap = next == &ytdlp_updater::on_program ? k_program_max : k_small_max;
	connect(r, &QNetworkReply::downloadProgress, r, [r, cap](qint64 got, qint64) {
		if (got > cap)
			r->abort();
	});
	connect(r, &QNetworkReply::finished, this, [this, r, next] {
		r->deleteLater();
		m_reply = nullptr;
		(this->*next)(r);
	});
}

void ytdlp_updater::on_latest(QNetworkReply *r) {
	const QUrl where = r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	m_tag = tag_from(r->url().resolved(where));
	if (m_tag.isEmpty())
		return done(false, false, QString("could not tell which yt-dlp release is the "
		                                  "latest: %1").arg(r->errorString()));
	get(QUrl(QString("%1/download/%2/SHA2-256SUMS").arg(QLatin1String(k_repo), m_tag)),
	    true, &ytdlp_updater::on_sums);
}

void ytdlp_updater::on_sums(QNetworkReply *r) {
	if (r->error() != QNetworkReply::NoError)
		return done(false, false, "could not fetch the release's checksums: " +
		                          r->errorString());
	m_sums = r->readAll();
	get(QUrl(QString("%1/download/%2/SHA2-256SUMS.sig").arg(QLatin1String(k_repo), m_tag)),
	    true, &ytdlp_updater::on_sig);
}

void ytdlp_updater::on_sig(QNetworkReply *r) {
	if (r->error() != QNetworkReply::NoError)
		return done(false, false, "could not fetch the checksums' signature: " +
		                          r->errorString());
	QString why;
	if (!verify(m_sums, r->readAll(), shipped_key(), &why))
		return done(false, false, why);
	m_want = sum_for(m_sums, "yt-dlp");
	if (m_want.isEmpty())
		return done(false, false, "the release lists no yt-dlp file");
	// Already this release, and the file is still the one it names.
	if (installed_version() == m_tag) {
		QFile have(QDir(directory()).filePath("yt-dlp"));
		if (have.open(QIODevice::ReadOnly) &&
		    QCryptographicHash::hash(have.readAll(), QCryptographicHash::Sha256).toHex() == m_want)
			return done(true, false, QString("yt-dlp %1 is the latest").arg(m_tag));
	}
	get(QUrl(QString("%1/download/%2/yt-dlp").arg(QLatin1String(k_repo), m_tag)), true,
	    &ytdlp_updater::on_program);
}

void ytdlp_updater::on_program(QNetworkReply *r) {
	if (r->error() != QNetworkReply::NoError)
		return done(false, false, "could not fetch yt-dlp " + m_tag + ": " + r->errorString());
	const QByteArray bytes = r->readAll();
	if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != m_want)
		return done(false, false, QString("yt-dlp %1 does not match its signed checksum, "
		                                  "so it was not used").arg(m_tag));
	if (!QDir().mkpath(directory()))
		return done(false, false, "could not make " + directory());
	// The program, then its version: a crash between the two leaves a
	// version naming the old file, which the next check finds and fixes,
	// rather than a version naming a file that is not there.
	QSaveFile out(QDir(directory()).filePath("yt-dlp"));
	if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
		return done(false, false, "could not write yt-dlp into " + directory());
	QSaveFile ver(QDir(directory()).filePath("version"));
	if (!ver.open(QIODevice::WriteOnly) || ver.write(m_tag.toUtf8()) < 0 || !ver.commit())
		return done(false, false, "could not record the yt-dlp version in " + directory());
	done(true, true, QString("yt-dlp updated to %1").arg(m_tag));
}

void ytdlp_updater::done(bool ok, bool updated, const QString &text) {
	m_ok      = ok;
	m_updated = updated;
	m_text    = text;
	ensure_impersonation();
}

void ytdlp_updater::run_step(const QString &program, const QStringList &args,
                             int timeout_ms,
                             void (ytdlp_updater::*next)(bool, const QString &)) {
	auto *p = new QProcess(this);
	m_proc = p;
	p->setProcessChannelMode(QProcess::MergedChannels);
	// Bounded: a pip that hangs on a network that never answers is killed,
	// and said, rather than holding the check open for ever.
	auto *limit = new QTimer(p);
	limit->setSingleShot(true);
	connect(limit, &QTimer::timeout, p, [p] { p->kill(); });
	connect(p, &QProcess::finished, this, [this, p, next](int code, QProcess::ExitStatus st) {
		const QString out = QString::fromUtf8(p->readAll());
		p->deleteLater();
		m_proc = nullptr;
		(this->*next)(st == QProcess::NormalExit && code == 0, out);
	});
	connect(p, &QProcess::errorOccurred, this, [this, p, next](QProcess::ProcessError e) {
		if (e != QProcess::FailedToStart)
			return;   // the others end in `finished` as well
		p->deleteLater();
		m_proc = nullptr;
		(this->*next)(false, p->errorString());
	});
	p->start(program, args);
	limit->start(timeout_ms);
}

void ytdlp_updater::ensure_impersonation() {
	m_had_python = !python().isEmpty();
	const QString venv = QDir(directory()).filePath("venv");
	if (QFileInfo::exists(QDir(venv).filePath("bin/python")))
		return on_venv(true, QString());
	const QString py = QStandardPaths::findExecutable("python3");
	if (py.isEmpty())
		return finish_all("no python3, so no impersonation for yt-dlp");
	QDir().mkpath(directory());
	run_step(py, { "-m", "venv", venv }, 120000, &ytdlp_updater::on_venv);
}

void ytdlp_updater::on_venv(bool ok, const QString &output) {
	if (!ok)
		return finish_all("could not make a Python environment for impersonation "
		                  "(is python3-venv installed?): " + output.trimmed().right(200));
	// Prebuilt wheels only: nothing is compiled on this machine, and a
	// release without one for this platform is skipped rather than built.
	const QStringList pip = { "-m", "pip", "install", "--disable-pip-version-check",
	                          "--no-input", "--quiet", "--only-binary=:all:",
	                          "--upgrade", "curl_cffi" };
	run_step(QDir(directory()).filePath("venv/bin/python"), pip, 300000,
	         &ytdlp_updater::on_pip);
}

void ytdlp_updater::on_pip(bool ok, const QString &output) {
	// Asked whether it imports, whichever way pip went: an environment that
	// had it keeps working when one day's upgrade fails.
	if (!ok)
		qWarning("yt-dlp: pip could not install curl_cffi: %s",
		         qPrintable(output.trimmed().right(300)));
	run_step(QDir(directory()).filePath("venv/bin/python"),
	         { "-c", "import curl_cffi; print(curl_cffi.__version__)" }, 60000,
	         &ytdlp_updater::on_import);
}

void ytdlp_updater::on_import(bool ok, const QString &output) {
	const QString marker = QDir(directory()).filePath("venv/impersonation-ok");
	if (!ok) {
		QFile::remove(marker);
		return finish_all("impersonation is not available: curl_cffi would not install");
	}
	QFile f(marker);
	if (f.open(QIODevice::WriteOnly))
		f.write(output.trimmed().toUtf8());
	finish_all("impersonation by curl_cffi " + output.trimmed());
}

void ytdlp_updater::finish_all(const QString &imp_text) {
	const bool changed = m_updated || (m_had_python != !python().isEmpty());
	const QString text = m_text + "; " + imp_text;
	QSettings s = state();
	s.setValue("ytdlp/last_result", text);
	emit finished(m_ok, changed, text);
}
