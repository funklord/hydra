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
	// Who is asking. **The two are not interchangeable and the dialog must not
	// let them look it.** A proxy prompt and a site prompt are the same box
	// with the same fields, and the credentials belong to different parties:
	// typing the site's password into the proxy's prompt hands it to the
	// network operator, who was never entitled to it. Naming the asker is the
	// only thing standing between those two.
	enum class asker { site, proxy };

	// `realm` is the asker's own label for what is being protected, and is
	// often empty or machine-generated. Shown when it says something, dropped
	// when it does not, rather than printing an empty pair of quotes.
	auth_dialog(const QString &host, const QString &realm, bool encrypted,
	             QWidget *parent = nullptr, asker who = asker::site);

	QString user() const;
	QString password() const;

private:
	QLineEdit *m_user = nullptr;
	QLineEdit *m_password = nullptr;
};
