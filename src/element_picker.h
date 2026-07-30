// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// What "zap this" captured (architecture doc §12.1).
struct picked_element {
	QString     selector;   // a CSS selector the page-side script derived
	QString     tag;        // lowercase tag name
	QString     id;
	QStringList classes;
	QString     snippet;    // trimmed outerHTML, for the AI's context
	QString     page_url;

	bool is_valid() const { return !tag.isEmpty(); }
};

// The user-driven half of filter-evolution signal collection (§12.1).
//
// This was deferred from step 6 for a structural reason: capturing a leaked
// ad's selector needs script injection and a channel into the page, which is
// the plumbing the password manager introduced (§13.2). It is that same seam,
// used for a second purpose.
//
// The C++ side stays deliberately thin — the page script does the picking, and
// everything it sends is treated as untrusted text that only ever becomes a
// *proposal* for the user to review, never a rule applied directly.
class element_picker : public QObject {
	Q_OBJECT
public:
	explicit element_picker(QObject *parent = nullptr);

	// Ask the page to enter pick mode. The script highlights on hover and
	// captures on click; Escape cancels.
	void begin(const QString &page_url);
	bool active() const { return m_active; }

	const picked_element &last() const { return m_last; }

public slots:
	// --- Reachable from the injected script. Treat the payload as hostile.
	void element_picked(const QString &json);
	void cancelled();

signals:
	// Tells the page-side script to start; the script connects to this.
	void pick_requested();
	void picked(const picked_element &element);
	void aborted();

private:
	picked_element m_last;
	QString        m_page_url;
	bool           m_active = false;
};
