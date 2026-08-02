// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_view.h"

#include <QLabel>

android_view::android_view(QWidget *parent) : web_view_backend(nullptr) {
	m_widget = new QLabel(parent);
	m_widget->setObjectName("android_placeholder");
	m_widget->setWordWrap(true);
	m_widget->setAlignment(Qt::AlignCenter);
	m_widget->setMargin(24);
	// The backend is owned by the widget, the same way the desktop one is, so
	// the shell can delete the widget and be done.
	setParent(m_widget);
	refresh();
}

QWidget *android_view::widget() {
	return m_widget;
}

void android_view::refresh() {
	m_widget->setText(
		QStringLiteral(
			"<h2>No web view on this platform yet</h2>"
			"<p>Everything else in Hydra is running: the tree, the policy "
			"engine, the download queue and the request filter are the same "
			"code as the desktop build.</p>"
			"<p>What is missing is the Android side of the WebView seam — a "
			"System WebView behind <tt>web_view_backend</tt>.</p>%1")
			.arg(m_url.isEmpty()
			         ? QString()
			         : QStringLiteral("<p>It was asked to open:<br><tt>%1</tt></p>")
			               .arg(m_url.toString().toHtmlEscaped())));
}

void android_view::load(const QUrl &url) {
	// Recorded and shown rather than silently dropped: the point of the
	// placeholder is that a screenshot of it says what happened.
	m_url = url;
	refresh();
	emit url_changed(url);
}

void android_view::inject_script(const QString &name, const QString &source,
                                  bool subframes) {
	Q_UNUSED(source)
	Q_UNUSED(subframes)
	m_scripts << name;
}

void android_view::inject_main_world_script(const QString &name,
                                             const QString &source) {
	Q_UNUSED(source)
	m_scripts << name;
}

void android_view::set_script_bridge(QObject *object, const QString &name) {
	// Deliberately nothing. On Android this becomes `addJavascriptInterface`,
	// which is a different mechanism with a different security model, and
	// pretending to register a bridge that cannot deliver would make every
	// script that waits for one hang instead of fail.
	Q_UNUSED(object)
	Q_UNUSED(name)
}

QByteArray android_view::save_state() const {
	// The address is all there is to keep. A real backend saves the WebView's
	// own back-forward list here.
	return m_url.toEncoded();
}

bool android_view::restore_state(const QByteArray &blob) {
	if (blob.isEmpty())
		return false;
	m_url = QUrl::fromEncoded(blob);
	refresh();
	return true;
}

web_view_backend *android_factory::create_view(QWidget *parent) {
	return new android_view(parent);
}
