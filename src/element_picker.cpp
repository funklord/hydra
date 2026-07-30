// SPDX-License-Identifier: GPL-3.0-or-later
#include "element_picker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// The snippet exists to give the model context, not to be rendered, so it is
// capped hard. §12.2 also asks for personal data to be stripped: the cheap,
// reliable version of that is to keep the element's shape and drop its text.
constexpr int k_max_snippet = 1200;

}  // namespace

element_picker::element_picker(QObject *parent) : QObject(parent) {}

void element_picker::begin(const QString &page_url) {
	m_page_url = page_url;
	m_active   = true;
	m_last     = picked_element{};
	emit pick_requested();
}

void element_picker::cancelled() {
	m_active = false;
	emit aborted();
}

void element_picker::element_picked(const QString &json) {
	m_active = false;

	const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
	picked_element e;
	e.tag      = o.value("tag").toString().toLower();
	e.id       = o.value("id").toString();
	e.selector = o.value("selector").toString();
	e.snippet  = o.value("snippet").toString().left(k_max_snippet);
	e.page_url = m_page_url;
	for (const QJsonValue &v : o.value("classes").toArray()) {
		const QString c = v.toString();
		if (!c.isEmpty())
			e.classes << c;
	}

	if (!e.is_valid()) {
		emit aborted();
		return;
	}
	m_last = e;
	emit picked(e);
}
