// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QSet>
#include <QString>

class download_manager;

// Making a finished download visible to the person who asked for it (§19).
//
// Qt's `DownloadLocation` on Android is app-private external storage. Writing
// there needs no permission and always works, which is why the download stack
// worked on a phone the day it was built — and it is also **invisible**: the
// Files app does not list it, no other app can open it, and it is deleted when
// Hydra is uninstalled. A browser whose downloads cannot be found afterwards has
// not really downloaded anything.
//
// So a completed file is copied into `MediaStore.Downloads`, which is the shared
// Downloads collection every file manager shows. That needs no permission
// either: an app may always insert its own entries. **This is a deliberate
// choice between two honest options** — the other is asking the user where to
// put each file with the Storage Access Framework, which is what a "save as"
// belongs to and what a browser should not do to every download.
//
// The original stays where it was. Copying rather than moving costs the space
// twice until the app is cleared, and buys a download that still works if the
// publish fails — a half-published file that had also been deleted from its
// first home would be a download that succeeded and then vanished.
class android_downloads : public QObject {
	Q_OBJECT
public:
	// Watches the manager and publishes each job as it completes. Does nothing
	// at all when the platform is older than the shared collection (API 29).
	android_downloads(download_manager *downloads, QObject *parent = nullptr);

	// The copy itself, without the watching. Returns the `content:` url the file
	// now lives at, or an empty string with `error` filled in.
	static QString publish(const QString &path, const QString &mime, QString *error);

signals:
	// So the shell can say where it went, in its own status bar rather than a
	// toast the browser does not control.
	void published(const QString &path, const QString &uri);
	void failed(const QString &path, const QString &message);

private:
	void poll();

	download_manager *m_downloads = nullptr;
	QSet<int>         m_done;   // job ids already handled, success or not
};
