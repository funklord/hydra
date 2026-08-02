// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class filter_list;

// The cosmetic half of the filter-evolution loop (architecture doc §12).
//
// A `##` rule hides an element rather than blocking a request, so it cannot ride
// the interceptor the way `||host^` does — it has to reach the page. This is the
// piece that takes it there: the shell tells it which host is on screen, an
// injected script asks it for that host's selectors, and the script writes them
// into a stylesheet.
//
// **It was missing entirely.** Accepted cosmetic rules were stored, listed and
// re-loaded, and hid nothing — the same gap the network half had, in the same
// loop, found by writing down that the network half was fixed and the cosmetic
// half was not.
//
// The page never names the host, exactly as with the consent bridge: the shell
// sets it on navigation, and `selectors_json()` answers for whatever that is.
// Otherwise any page could ask what rules exist for any site, which is a small
// leak of what the user has been doing.
class cosmetic_filters : public QObject {
	Q_OBJECT
public:
	explicit cosmetic_filters(const filter_list *list, QObject *parent = nullptr)
	    : QObject(parent), m_list(list) {}

	// The object name the injected script expects on the bridge.
	static const char *bridge_name() { return "hydraCosmetic"; }
	static QString script_source();

	// Set by the shell on navigation. Never by the page.
	void set_page_host(const QString &host);

	// The selectors for one host, without the bridge. Shared with the tests, and
	// with anything that wants to know what would be hidden without asking a
	// live page.
	static QStringList selectors_for(const filter_list *list, const QString &host);

	// For HYDRA_FILTER_DEBUG only.
	QString debug_state() const;

public slots:
	// A JSON array of CSS selectors for the host currently on screen.
	QString selectors_json() const;

private:
	const filter_list *m_list = nullptr;
	QString            m_host;
};
