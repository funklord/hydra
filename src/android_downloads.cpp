// **Guarded here, not only by the build system.** `hydra.pro` drops this file
// outside `android {}`, and until it also said so itself the file depended on a
// build file to be correct: anything reading the tree rather than the build --
// an indexer, a language server, a static analyser, `fmake` -- reached
// `<QJniEnvironment>` on a desktop and stopped. Measured, not imagined.
//
// `<QtGlobal>` first, and it is load-bearing: the compiler defines `__ANDROID__`
// but **`Q_OS_ANDROID` is Qt's**, so testing it before any Qt header is included
// is false everywhere -- including on Android, where it would empty this file in
// the one build that needs it.
#include <QtGlobal>
#ifdef Q_OS_ANDROID

#include "android_downloads.h"
#include "download_manager.h"
#include "media_detector.h"

#include <QFileInfo>
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

namespace {

const char *k_cls = "se/vibes/hydra/HydraDownloads";

}  // namespace

android_downloads::android_downloads(download_manager *downloads, QObject *parent)
    : QObject(parent), m_downloads(downloads) {
	if (!m_downloads)
		return;
	// `changed()` is the only signal there is -- the manager reports that
	// something moved, not which job finished -- so completion is worked out here
	// rather than asked for. A set of ids already handled keeps it to once each,
	// which matters because `changed()` fires on every progress tick.
	connect(m_downloads, &download_manager::changed, this, &android_downloads::poll);
}

void android_downloads::poll() {
	for (const download_job &j : m_downloads->jobs()) {
		// **Only a download this session watched finish.** The handled set
		// used to start empty each launch, and the download history brings
		// back every finished job as finished -- so each one was published
		// again at every start, and the shared Downloads folder gained a
		// copy of everything ever downloaded per launch: 17 of one file on
		// the emulator, 2026-10-29. A job is published only if it was seen
		// unfinished first, which a new one always is -- enqueueing it is
		// itself a `changed()` -- and a restored one never is.
		if (j.status != download_state::done) {
			if (!j.terminal())
				m_running.insert(j.id);
			continue;
		}
		if (!m_running.remove(j.id))
			continue;
		if (j.path.isEmpty() || !QFileInfo::exists(j.path))
			continue;   // a job with no file of its own: nothing to publish

		QString error;
		const QString uri = publish(j.path, media_mime_for(j.url), &error);
		if (uri.isEmpty())
			emit failed(j.path, error);
		else
			emit published(j.path, uri);
	}
}

QString android_downloads::publish(const QString &path, const QString &mime,
                                    QString *error) {
	const QJniObject result = QJniObject::callStaticObjectMethod(
	  k_cls, "publish",
	  "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)"
	  "Ljava/lang/String;",
	  QNativeInterface::QAndroidApplication::context().object(),
	  QJniObject::fromString(path).object<jstring>(),
	  QJniObject::fromString(mime).object<jstring>());

	if (QJniEnvironment().checkAndClearExceptions()) {
		if (error)
			*error = "This build has no Android downloads helper.";
		return QString();
	}
	const QString uri = result.isValid() ? result.toString() : QString();
	if (uri.isEmpty() && error)
		*error = "Could not copy the file into the shared Downloads folder.";
	return uri;
}
#endif   // Q_OS_ANDROID
