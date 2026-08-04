// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_mirror.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QTimer>

session_mirror::session_mirror(QObject *parent) : QObject(parent) {
	m_timer = new QTimer(this);
	connect(m_timer, &QTimer::timeout, this, [this] { poll_once(); });
}

void session_mirror::start(const QString &session_file, int interval_ms) {
	m_path = session_file;
	// Forget what the last source looked like, or switching profiles would look
	// like "nothing changed" and show the previous browser's tabs for ever.
	m_fingerprint.clear();
	m_last_error.clear();
	m_size  = -1;
	m_mtime = QDateTime();
	m_timer->start(qMax(1000, interval_ms));
	// Once immediately, so turning it on does something visible rather than
	// leaving the user to wonder for a whole interval whether it worked.
	poll_once();
}

void session_mirror::stop() { m_timer->stop(); }
bool session_mirror::running() const { return m_timer->isActive(); }

QString session_mirror::fingerprint(const QList<session_import::imported_tab> &tabs) {
	// Address and title, in order. Not the window index: a tab dragged between
	// two Firefox windows is the same tab and rebuilding the mirror for it
	// would be churn. Not the count alone either -- closing one tab and opening
	// another is a change that a count would miss entirely.
	QCryptographicHash h(QCryptographicHash::Sha1);
	for (const session_import::imported_tab &t : tabs) {
		h.addData(t.url.toUtf8());
		h.addData(QByteArrayView("\x1f", 1));
		h.addData(t.title.toUtf8());
		h.addData(QByteArrayView("\x1e", 1));
	}
	return QString::fromLatin1(h.result().toHex());
}

bool session_mirror::poll_once() {
	if (m_path.isEmpty())
		return false;

	// The cheap question first. This runs every interval for as long as the
	// feature is on, and decompressing five megabytes to discover that nothing
	// happened is not a thing to do four times a minute.
	const QFileInfo fi(m_path);
	if (!fi.exists()) {
		const QString why = "The session file is gone: " + m_path;
		if (why != m_last_error) {
			m_last_error = why;
			emit failed(why);
		}
		return false;
	}
	if (fi.size() == m_size && fi.lastModified() == m_mtime)
		return false;   // untouched since the last look
	m_size  = fi.size();
	m_mtime = fi.lastModified();

	QString error;
	const QList<session_import::imported_tab> tabs =
		session_import::firefox_tabs(m_path, &error);
	if (tabs.isEmpty()) {
		// A browser mid-write can hand back a partial file, so this is not
		// necessarily wrong -- it is only worth saying once, and the next poll
		// will find a whole one.
		if (!error.isEmpty() && error != m_last_error) {
			m_last_error = error;
			emit failed(error);
		}
		return false;
	}
	m_last_error.clear();

	// The expensive question, and the one that decides whether anything moves.
	// Firefox rewrites this file for a scroll offset; the tab set is what the
	// user would call a change.
	const QString fp = fingerprint(tabs);
	if (fp == m_fingerprint)
		return false;
	m_fingerprint = fp;
	emit tabs_changed(tabs);
	return true;
}
