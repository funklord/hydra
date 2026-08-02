// SPDX-License-Identifier: GPL-3.0-or-later
#include "scheme_rules.h"

#include <QSet>
#include <QString>
#include <QUrl>

bool renders_as_page(const QUrl &url) {
	static const QSet<QString> web = {
		"http", "https", "file", "about", "data", "blob",
		"view-source", "chrome", "qrc",
	};
	return web.contains(url.scheme().toLower());
}
