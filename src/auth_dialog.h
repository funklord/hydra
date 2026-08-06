// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;

// The username and password a site is asking for.
//
// **A site wanting HTTP authentication could not be used at all.** Qt reports
// the challenge through a callback that has to be answered while it runs, and
// nothing answered it -- so the request was abandoned and the page failed, with
// nothing on screen to say a password had been asked for.
//
// It names the site, because that is the one thing a person must check before
// typing a password anywhere, and it says so plainly when the connection is not
// encrypted: HTTP authentication over a plain connection puts the password on
// the wire in a form anyone between here and there can read.
class auth_dialog : public QDialog {
	Q_OBJECT
public:
	// `realm` is the site's own label for what is being protected, and is
	// often empty or machine-generated. Shown when it says something, dropped
	// when it does not, rather than printing an empty pair of quotes.
	auth_dialog(const QString &host, const QString &realm, bool encrypted,
	             QWidget *parent = nullptr);

	QString user() const;
	QString password() const;

private:
	QLineEdit *m_user = nullptr;
	QLineEdit *m_password = nullptr;
};
