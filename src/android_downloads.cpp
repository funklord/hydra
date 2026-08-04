// SPDX-License-Identifier: GPL-3.0-or-later
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
	// `changed()` is the only signal there is — the manager reports that
	// something moved, not which job finished — so completion is worked out here
	// rather than asked for. A set of ids already handled keeps it to once each,
	// which matters because `changed()` fires on every progress tick.
	connect(m_downloads, &download_manager::changed, this, &android_downloads::poll);
}

void android_downloads::poll() {
	for (const download_job &j : m_downloads->jobs()) {
		if (j.status != download_state::done || m_done.contains(j.id))
			continue;
		m_done.insert(j.id);
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
