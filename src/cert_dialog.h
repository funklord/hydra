// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web_view_backend.h"

#include <QDialog>
#include <QList>

class QListWidget;

// Which client certificate to send a site, if any.
//
// **Sending one is an act of identification**, and that is what makes this a
// question rather than a setting. A client certificate names its holder to the
// site in a way a password does not: the site learns who you are before you
// have decided to tell it, and there is no taking it back afterwards.
//
// So the safe answer is the reachable one. `Don't send` is the default button
// and the escape key's answer, and the dialog opens with nothing selected. That
// also matches what happened before this existed: Qt aborts a certificate
// selection nothing is connected to, so "none" was already the outcome -- the
// difference is that it is now a choice somebody made.
//
// The site is named for the same reason the password dialog names it: it is the
// one thing that has to be checked before answering.
class cert_dialog : public QDialog {
	Q_OBJECT
public:
	cert_dialog(const QString &host,
	             const QList<web_view_backend::certificate_offer> &offered,
	             QWidget *parent = nullptr);

	// The index into the list that was offered, or -1 for "send none". A
	// rejected dialog answers -1, so a closed window and a pressed `Don't
	// send` mean the same thing rather than differing by accident.
	int chosen() const;

private:
	QListWidget *m_list = nullptr;
	bool m_send = false;
};
