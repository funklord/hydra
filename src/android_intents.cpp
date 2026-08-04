// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_intents.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

namespace {

const char *k_cls = "org/qtproject/example/hydra/HydraIntents";

}  // namespace

namespace android_intents {

bool open_media(const QUrl &url, const QString &mime, QString *error) {
	if (!url.isValid()) {
		if (error)
			*error = "Nothing to play.";
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(
		k_cls, "openMedia",
		"(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)Z",
		QNativeInterface::QAndroidApplication::context().object(),
		QJniObject::fromString(url.toString()).object<jstring>(),
		QJniObject::fromString(mime).object<jstring>());

	// A missing class throws rather than returning, so the exception state is
	// the answer to "is the Java side even in this APK".
	if (QJniEnvironment().checkAndClearExceptions()) {
		if (error)
			*error = "This build has no Android intent helper.";
		return false;
	}
	if (ok == JNI_FALSE) {
		if (error)
			*error = "No app on this device offered to open the stream.";
		return false;
	}
	return true;
}

bool open_externally(const QUrl &url, QString *error) {
	if (!url.isValid() || url.scheme().startsWith("about")) {
		if (error)
			*error = "There is no address to hand over.";
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(
		k_cls, "openExternally",
		"(Landroid/app/Activity;Ljava/lang/String;)Z",
		QNativeInterface::QAndroidApplication::context().object(),
		QJniObject::fromString(url.toString()).object<jstring>());

	if (QJniEnvironment().checkAndClearExceptions()) {
		if (error)
			*error = "This build has no Android intent helper.";
		return false;
	}
	if (ok == JNI_FALSE) {
		// Worth distinguishing from the media case: there, nothing could play a
		// stream; here, nothing claims the address at all, which on a bare
		// emulator image means no browser and no app for that site.
		if (error)
			*error = "No app on this device offered to open that address.";
		return false;
	}
	return true;
}

}  // namespace android_intents
