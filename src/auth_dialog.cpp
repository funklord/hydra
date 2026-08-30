#include "auth_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

auth_dialog::auth_dialog(const QString &host, const QString &realm,
                          bool encrypted, QWidget *parent, asker who)
    : QDialog(parent) {
	const bool proxy = who == asker::proxy;
	setWindowTitle(proxy ? "Sign in to the proxy" : "Sign in");
	setObjectName("auth_dialog");
	auto *column = new QVBoxLayout(this);

	auto *asking = new QLabel(this);
	asking->setObjectName("auth_site");
	asking->setWordWrap(true);
	// Held in locals rather than nested in the call: the sentence differs by
	// asker and the name differs by whether there was one, and the two
	// conditions read as two questions when they are not folded together.
	const QString sentence =
	  proxy ? QStringLiteral("The proxy <b>%1</b> is asking for a username "
	                          "and password.")
	         : QStringLiteral("<b>%1</b> is asking for a username and "
	                           "password.");
	const QString named =
	  !host.isEmpty() ? host
	  : proxy         ? QStringLiteral("this network uses")
	                  : QStringLiteral("This site");
	asking->setText(sentence.arg(named));
	column->addWidget(asking);

	// **Which password is being asked for.** The prompt is otherwise identical
	// to the site's, and the two are answered from different places: this one
	// belongs to the network being reached through, not to the page on screen.
	// Said plainly, because the cost of confusing them is handing a site's
	// password to whoever runs the proxy.
	if (proxy) {
		auto *whose = new QLabel(this);
		whose->setObjectName("auth_proxy_note");
		whose->setWordWrap(true);
		whose->setText("This is the network you are connecting through, not the "
		                "site you are visiting. Do not use the site's password.");
		column->addWidget(whose);
	}

	// The realm is the site's own name for what it is protecting. Often empty,
	// and often a machine-generated string that means nothing to anybody, so it
	// is shown only when it says something.
	if (!realm.trimmed().isEmpty()) {
		auto *for_what = new QLabel(QString("For: %1").arg(realm), this);
		for_what->setObjectName("auth_realm");
		for_what->setWordWrap(true);
		column->addWidget(for_what);
	}

	// **The one thing worth interrupting for.** HTTP authentication over a
	// plain connection sends the password in a form anything between here and
	// the site can read, and the person typing it is the only one who can
	// decide whether that is acceptable. Said in the dialog rather than after
	// the fact, because after the fact the password has already gone.
	if (!encrypted) {
		auto *warning = new QLabel(this);
		warning->setObjectName("auth_insecure");
		warning->setWordWrap(true);
		warning->setText(proxy
		                     ? "<b>This connection is not encrypted.</b> A "
		                       "password sent to the proxy can be read by "
		                       "anything on the way to it."
		                     : "<b>This connection is not encrypted.</b> A "
		                       "password sent to it can be read by anything "
		                       "between you and the site.");
		column->addWidget(warning);
	}

	m_user = new QLineEdit(this);
	m_user->setObjectName("auth_user");
	m_user->setPlaceholderText("Username");
	column->addWidget(m_user);

	m_password = new QLineEdit(this);
	m_password->setObjectName("auth_password");
	m_password->setPlaceholderText("Password");
	m_password->setEchoMode(QLineEdit::Password);
	column->addWidget(m_password);

	// **Spare height goes here, not between the sentences.** On a desktop the
	// dialog is whatever height its contents ask for and this changes nothing.
	// On Android it is handed the whole screen, and with nothing willing to
	// absorb the difference a word-wrapped label takes a share of it -- so the
	// prompt arrived with an inch of nothing between each line and the two
	// fields pushed to the bottom edge, which reads as a broken window rather
	// than a question.
	column->addStretch(1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
	                                      QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText("Sign &in");
	column->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	m_user->setFocus();
}

QString auth_dialog::user() const { return m_user->text(); }
QString auth_dialog::password() const { return m_password->text(); }
