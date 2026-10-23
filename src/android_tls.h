#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QPluginLoader>
#include <QSslSocket>
#include <QStringList>

// **Load Qt's OpenSSL backend by hand, on Android.** With OpenSSL in the
// package and unpacked, Qt still did not load its own TLS plugin there:
// `availableBackends()` listed only "cert-only" and every HTTPS request
// failed, in bbq-predictor first and so here, where it left the phone with
// no filter lists and so no ad blocking (2026-10-23). The plugin sits in
// the app's library directory, which is not on `libraryPaths()`, and it
// registers itself only when its root object is constructed -- so
// `instance()`, not `load()`. bbq-predictor's `src/net/tls_backend.cpp`
// has the measurements behind each of those. Header-only: one call, from
// `main`, before anything makes a request.
namespace android_tls {

inline void ensure_backend() {
	if (QSslSocket::supportsSsl())
		return;
	QStringList places = QCoreApplication::libraryPaths();
	places << QCoreApplication::applicationDirPath()
	       << QLibraryInfo::path(QLibraryInfo::PluginsPath);
	places.removeDuplicates();
	for (const QString &path : places) {
		const QDir dir(path);
		for (const QString &name : dir.entryList({ QStringLiteral("*tls*openssl*") }, QDir::Files)) {
			QPluginLoader loader(dir.absoluteFilePath(name));
			if (loader.instance() && QSslSocket::supportsSsl()) {
				qInfo("tls: %s, from %s", qPrintable(QSslSocket::sslLibraryVersionString()),
				      qPrintable(name));
				return;
			}
		}
	}
	// Said, since without it nothing hydra fetches itself can arrive:
	// filter lists, AI requests.
	qWarning("tls: no TLS backend could be loaded, so hydra's own requests will fail; "
	         "was the package built after tool/build-openssl-android.sh?");
}

}  // namespace android_tls
