#pragma once

#include "filter_list.h"
#include "investigation.h"

#include <QObject>
#include <QPointer>

class cosmetic_filters;
class request_filter;
class web_view_backend;

// **An investigation's hands on one tab**: `investigation_host` against a
// real view. Looks with `ad_probe`; tries rules by giving the tab trial lists
// of each kind and reloading it; hands questions to whoever owns the window.
//
// Trial rules reach three places, each the narrowest it can be: cosmetic ones
// through this tab's own cosmetic bridge, scriptlets as a script on this tab
// alone, and network ones through the request filter for this site only --
// the one route that is per site rather than per tab, because requests are
// filtered for the whole profile and carry the site, not the tab.
//
// **Nothing outlives it.** `end` takes every trial rule back out and reloads,
// and the destructor does the same, so a window closed in any way cannot leave
// a trial behind.
class tab_investigation : public QObject, public investigation_host {
	Q_OBJECT
public:
	tab_investigation(web_view_backend *view, request_filter *filter,
	                  const QString &site_host, QObject *parent = nullptr);
	~tab_investigation() override;

	// Set by the window: how a question reaches the person.
	using asker = std::function<void(const QString &kind, const QString &question,
	                                 std::function<void(const QString &)> done)>;
	void set_asker(asker fn) { m_asker = std::move(fn); }

	void look(std::function<void(const QStringList &)> done) override;
	void trial(const QList<filter_rule> &rules,
	           const QList<scriptlet_call> &calls,
	           std::function<void(const QStringList &)> done) override;
	void ask(const QString &kind, const QString &question,
	         std::function<void(const QString &)> done) override;

	// Take every trial rule out, and reload when anything was in trial.
	void end();
	bool has_trial() const { return m_in_trial; }

	// How long the page is given to settle after a reload before it is looked
	// at, in milliseconds: players and late ad slots arrive after the load.
	static constexpr int k_settle_ms = 2500;

private:
	void clear(bool reload);

	QPointer<web_view_backend> m_view;
	request_filter            *m_filter = nullptr;
	QPointer<cosmetic_filters> m_cosmetic;
	QString                    m_site;
	filter_list                m_cosmetic_trial;
	asker                      m_asker;
	bool                       m_in_trial = false;
};
