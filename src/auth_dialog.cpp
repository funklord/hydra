// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

auth_dialog::auth_dialog(const QString &host, const QString &realm,
                          bool encrypted, QWidget *parent)
    : QDialog(parent) {
	setWindowTitle("Sign in");
	setObjectName("auth_dialog");
	auto *column = new QVBoxLayout(this);

	auto *asking = new QLabel(this);
	asking->setObjectName("auth_site");
	asking->setWordWrap(true);
	asking->setText(QString("<b>%1</b> is asking for a username and password.")
	                    .arg(host.isEmpty() ? QStringLiteral("This site") : host));
	column->addWidget(asking);

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
		warning->setText("<b>This connection is not encrypted.</b> A password "
		                  "sent to it can be read by anything between you and "
		                  "the site.");
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
