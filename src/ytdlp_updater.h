#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;


// Keeps yt-dlp current after install, from yt-dlp's own signed releases.
//
// **Why it exists**: the package ships yt-dlp pinned, and it ages there --
// the holder found it three months old on 2026-10-22, and YouTube refuses
// old releases outright. Debian's copy is older still, and yt-dlp's own
// `-U` updates only its release builds, not a source copy like the one
// shipped. So, by the holder's choice, hydra fetches the release itself,
// at most once a day and only as a lookup is about to run, and never on
// Android, where there is no yt-dlp.
//
// **What it trusts, and how it checks.** A release's `SHA2-256SUMS` is
// signed with yt-dlp's release key, which ships with the vendored copy as
// `public.key`. The signature is checked with `gpgv` against that key
// alone, and the signer's fingerprint must be the one written below. The
// downloaded `yt-dlp` -- their platform-independent release, a zip python3
// runs directly -- must match its line in that file. Anything short of all
// of it, and the copy already there is kept.
//
// The update lands in the profile, `<app data>/yt-dlp/`, never beside the
// installed program, which a user cannot write and the package owns.
class ytdlp_updater : public QObject {
	Q_OBJECT
public:
	explicit ytdlp_updater(QObject *parent = nullptr);

	// yt-dlp's release signing key, as `public.key` names it.
	static constexpr const char *k_fingerprint = "AC0CBBE6848D6A873464AF4E57CF65933B5A7581";

	// Where an update is kept, and what is there: the release file and the
	// version it is. Empty when there is none.
	static QString directory();
	static QString program();
	static QString installed_version();

	// Check for a newer release. Without `forced`, at most once a day: a
	// check that ran in the last 24 hours, finished or not, is not repeated.
	void check(bool forced = false);
	bool busy() const { return m_reply != nullptr; }

	// --- pure, and separately testable ---------------------------------
	// The release a `releases/tag/<tag>` address names, or empty.
	static QString tag_from(const QUrl &url);
	// A file's sha256 from a `SHA2-256SUMS` body, as lowercase hex, or empty.
	static QByteArray sum_for(const QByteArray &sums, const QString &file);
	// An ASCII-armoured key, as the binary packets `gpgv` reads.
	static QByteArray dearmor(const QByteArray &armored);
	// Whether `sig` is a good signature over `data` by the key in `armored`,
	// and that key is `k_fingerprint`. Runs `gpgv`; false with `why` when
	// it is missing, the signature is bad, or the signer is another key.
	static bool verify(const QByteArray &data, const QByteArray &sig,
	                   const QByteArray &armored, QString *why);
	// The release key shipped with the vendored copy, or empty.
	static QByteArray shipped_key();

signals:
	// `updated`: a newer copy is now in place. `text` says what happened,
	// for a settings page or a log.
	void finished(bool ok, bool updated, const QString &text);

private:
	void get(const QUrl &url, bool follow, void (ytdlp_updater::*next)(QNetworkReply *));
	void on_latest(QNetworkReply *r);
	void on_sums(QNetworkReply *r);
	void on_sig(QNetworkReply *r);
	void on_program(QNetworkReply *r);
	void done(bool ok, bool updated, const QString &text);

	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QString    m_tag;
	QByteArray m_sums;
	QByteArray m_want;   // the sha256 the release's sums give its yt-dlp
};
