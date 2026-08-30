#include "webauth_dialog.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

// Who is being signed in to, in a form that reads in a sentence. The relying
// party id is a domain and is normally there; an empty one is possible and must
// not produce a sentence with a hole in it.
QString site_or_this_site(const QString &relying_party) {
	return relying_party.isEmpty() ? QStringLiteral("this site") : relying_party;
}

// What went wrong, said in a way somebody can act on.
//
// Qt names the reason and stops there -- `KeyNotRegistered`, `SoftPinBlock` --
// and the half it leaves out is the half that matters at the moment a sign-in
// has just failed: whether this is worth another go, and with what. The two
// PIN-block reasons are the pair to get right, because they look alike and one
// of them is recoverable by unplugging the key while the other has destroyed
// access to it.
QString failure_text(webauth_dialog::failure why, const QString &relying_party) {
	const QString site = site_or_this_site(relying_party);
	switch (why) {
		case webauth_dialog::failure::timeout:
			return QStringLiteral("Nothing answered in time, so the request was "
			                       "abandoned. Start signing in again to have "
			                       "another go.");
		case webauth_dialog::failure::key_not_registered:
			return QStringLiteral("This security key is not registered with %1. "
			                       "Try the one you enrolled.").arg(site);
		case webauth_dialog::failure::key_already_registered:
			return QStringLiteral("This security key is already registered with "
			                       "%1. Use a different one.").arg(site);
		case webauth_dialog::failure::soft_pin_block:
			return QStringLiteral("The security key locked itself after too many "
			                       "wrong PINs. Unplug it, plug it back in, and "
			                       "try again.");
		case webauth_dialog::failure::hard_pin_block:
			return QStringLiteral("The security key locked itself after too many "
			                       "wrong PINs and will not unlock. Resetting it "
			                       "is the only way back, and a reset erases "
			                       "every passkey on it.");
		case webauth_dialog::failure::removed_during_pin_entry:
			return QStringLiteral("The security key was removed while the PIN was "
			                       "being entered. Put it back and start signing "
			                       "in again.");
		case webauth_dialog::failure::no_resident_keys:
			return QStringLiteral("This security key cannot store a passkey of its "
			                       "own, which is what %1 asked for.").arg(site);
		case webauth_dialog::failure::no_user_verification:
			return QStringLiteral("This security key cannot check that it is you "
			                       "-- it has no PIN and no fingerprint -- and %1 "
			                       "asked it to.").arg(site);
		case webauth_dialog::failure::no_large_blob:
			return QStringLiteral("This security key does not have the extra "
			                       "storage %1 asked for.").arg(site);
		case webauth_dialog::failure::no_common_algorithms:
			return QStringLiteral("This security key and %1 have no signature "
			                       "algorithm in common.").arg(site);
		case webauth_dialog::failure::storage_full:
			return QStringLiteral("The security key has no room for another "
			                       "passkey. Remove one you no longer use.");
		case webauth_dialog::failure::consent_denied:
			return QStringLiteral("The request was refused at the security key.");
		case webauth_dialog::failure::cancelled_by_system:
			return QStringLiteral("The system's own sign-in window was closed, so "
			                       "the request was withdrawn.");
		case webauth_dialog::failure::unknown:
			break;
	}
	// The `unknown` arm, and anything a later engine adds that this build was
	// compiled before. Says that it stopped rather than pretending to know why,
	// because a window with no sentence in it is the failure this whole file
	// exists to remove.
	return QStringLiteral("Signing in stopped, for a reason this browser has no "
	                       "name for.");
}

// Which failures a retry can do anything about.
//
// **Taken from Qt's own simplebrowser example rather than guessed**: `retry()`
// asks the engine to run the request again, and offering it where nothing has
// changed is a button that fails identically every time. The example offers it
// for exactly two reasons, and one omission in it looks wrong rather than
// deliberate -- `AuthenticatorRemovedDuringPinEntry` tells the person to
// reinsert and try again and then gives them no way to. That is copied as it
// stands rather than corrected here, because a retry the engine is not expecting
// is worse than a sentence that over-promises, and the discrepancy belongs to
// the project that wrote it.
bool retryable(webauth_dialog::failure why) {
	return why == webauth_dialog::failure::key_already_registered
	    || why == webauth_dialog::failure::soft_pin_block;
}

// What the key said about the last PIN, or nothing when it said nothing.
QString pin_error_text(webauth_dialog::pin_error error) {
	switch (error) {
		case webauth_dialog::pin_error::none:
			return QString();
		case webauth_dialog::pin_error::uv_locked:
			return QStringLiteral("The key stopped accepting fingerprints after "
			                       "too many tries. Type its PIN instead.");
		case webauth_dialog::pin_error::wrong_pin:
			return QStringLiteral("That PIN was wrong.");
		case webauth_dialog::pin_error::too_short:
			return QStringLiteral("That PIN is shorter than the key allows.");
		case webauth_dialog::pin_error::invalid_characters:
			return QStringLiteral("That PIN has characters the key will not take.");
		case webauth_dialog::pin_error::same_as_current:
			return QStringLiteral("The new PIN has to differ from the current one.");
	}
	return QString();
}

}  // namespace

webauth_dialog::webauth_dialog(const QString &relying_party, QWidget *parent)
    : QDialog(parent), m_relying_party(relying_party) {
	setWindowTitle("Passkey");
	setObjectName("webauth_dialog");
	setMinimumWidth(460);

	auto *column = new QVBoxLayout(this);

	m_heading = new QLabel(this);
	m_heading->setObjectName("webauth_heading");
	m_heading->setWordWrap(true);
	column->addWidget(m_heading);

	m_detail = new QLabel(this);
	m_detail->setObjectName("webauth_detail");
	m_detail->setWordWrap(true);
	column->addWidget(m_detail);

	// **Scrolled, because the number of passkeys is the site's to decide.** A
	// person with a dozen enrolled on one domain would otherwise get a window
	// taller than the screen, with the buttons off the bottom of it -- and the
	// buttons are the only way to answer or to get out.
	m_accounts = new QWidget;
	m_accounts->setObjectName("webauth_accounts");
	auto *account_rows = new QVBoxLayout(m_accounts);
	// One stretch, added once and never removed, with every choice inserted
	// above it. Adding a stretch per rebuild is how a list that is emptied and
	// refilled ends up with the names pushed further down the panel each time.
	account_rows->addStretch(1);
	m_accounts_area = new QScrollArea(this);
	m_accounts_area->setObjectName("webauth_accounts_area");
	m_accounts_area->setWidgetResizable(true);
	m_accounts_area->setWidget(m_accounts);
	column->addWidget(m_accounts_area);

	m_account_group = new QButtonGroup(this);

	m_pin_box = new QWidget(this);
	m_pin_box->setObjectName("webauth_pin_box");
	auto *pin_rows = new QFormLayout(m_pin_box);
	m_pin = new QLineEdit(m_pin_box);
	m_pin->setObjectName("webauth_pin");
	// Echoed as dots, like every other secret this program takes. A security
	// key's PIN unlocks every passkey stored on it, so it is not a lesser
	// secret than the password field two dialogs over.
	m_pin->setEchoMode(QLineEdit::Password);
	pin_rows->addRow("PIN", m_pin);
	m_confirm = new QLineEdit(m_pin_box);
	m_confirm->setObjectName("webauth_confirm");
	m_confirm->setEchoMode(QLineEdit::Password);
	m_confirm_label = new QLabel("Confirm PIN", m_pin_box);
	m_confirm_label->setObjectName("webauth_confirm_label");
	pin_rows->addRow(m_confirm_label, m_confirm);
	m_pin_trouble = new QLabel(m_pin_box);
	m_pin_trouble->setObjectName("webauth_pin_trouble");
	m_pin_trouble->setWordWrap(true);
	pin_rows->addRow(m_pin_trouble);
	column->addWidget(m_pin_box);

	m_buttons = new QDialogButtonBox(this);
	m_buttons->setObjectName("webauth_buttons");
	m_accept = m_buttons->addButton(QDialogButtonBox::Ok);
	m_retry  = m_buttons->addButton(QDialogButtonBox::Retry);
	m_cancel = m_buttons->addButton(QDialogButtonBox::Cancel);
	column->addWidget(m_buttons);

	// **The accept button does not close the window**, which is the one place
	// this differs from every other dialog in the tree. A PIN that the key
	// rejects comes straight back as another PIN question, and an account
	// choice is followed by a request to touch the key: closing on accept would
	// throw away the window the next state needs and leave the engine talking
	// to nothing. The window closes when the caller takes it away, and at no
	// other time.
	connect(m_accept, &QAbstractButton::clicked, this, [this] {
		switch (m_asked) {
			case asked::account: {
				QAbstractButton *picked = m_account_group->checkedButton();
				if (picked)
					emit account_chosen(picked->text());
				break;
			}
			case asked::pin:
				emit pin_entered(m_pin->text());
				break;
			case asked::nothing:
			case asked::touch:
			case asked::trouble:
				break;
		}
	});
	connect(m_retry, &QAbstractButton::clicked, this,
	         [this] { emit retry_requested(); });
	// Cancel, Escape and the window's close button all arrive here, because
	// QDialog routes the last two through `reject()`. One path, so there is no
	// way to shut this window without the engine being told.
	connect(m_cancel, &QAbstractButton::clicked, this, &QDialog::reject);
	connect(this, &QDialog::rejected, this, &webauth_dialog::cancelled);

	connect(m_pin, &QLineEdit::textChanged, this,
	         [this] { refresh_accept(); });
	connect(m_confirm, &QLineEdit::textChanged, this,
	         [this] { refresh_accept(); });
	// Qt 6 spells this `idToggled`; `buttonToggled` is the 5.x name and is gone.
	connect(m_account_group, &QButtonGroup::idToggled, this,
	         [this](int, bool) { refresh_accept(); });
	// The PIN field is the only one with a natural Return: pressing it should
	// submit rather than do nothing, which is what a form with a disabled
	// default button does.
	connect(m_pin, &QLineEdit::returnPressed, this, [this] {
		if (m_accept->isEnabled())
			m_accept->click();
	});
	connect(m_confirm, &QLineEdit::returnPressed, this, [this] {
		if (m_accept->isEnabled())
			m_accept->click();
	});

	clear_to_empty();
}

void webauth_dialog::clear_to_empty() {
	m_asked = asked::nothing;
	m_min_pin = 0;
	m_confirming = false;

	// The radio buttons are built per request, so they are destroyed per
	// request. A set left standing offers names from a sign-in that has already
	// moved on, and picking one of those answers the wrong question.
	const QList<QAbstractButton *> standing = m_account_group->buttons();
	for (QAbstractButton *button : standing) {
		m_account_group->removeButton(button);
		delete button;
	}
	m_accounts_area->setVisible(false);

	m_pin->clear();
	m_confirm->clear();
	m_pin_trouble->clear();
	m_pin_box->setVisible(false);

	m_accept->setVisible(false);
	m_retry->setVisible(false);
	m_cancel->setVisible(true);
	m_cancel->setText("Cancel");
	m_heading->clear();
	m_detail->clear();
}

void webauth_dialog::refresh_accept() {
	switch (m_asked) {
		case asked::account:
			m_accept->setEnabled(m_account_group->checkedButton() != nullptr);
			return;
		case asked::pin: {
			const QString typed = m_pin->text();
			// Non-empty, long enough for the key if the key said how long, and
			// matching the confirmation when there is one. Checked here rather
			// than on click so the button never offers to send something the
			// key is going to refuse -- and a refused PIN costs one of a small
			// number of tries before the key locks itself.
			bool usable = !typed.isEmpty() && typed.size() >= m_min_pin;
			if (usable && m_confirming)
				usable = typed == m_confirm->text();
			m_accept->setEnabled(usable);
			return;
		}
		case asked::nothing:
		case asked::touch:
		case asked::trouble:
			return;
	}
}

void webauth_dialog::ask_for_account(const QStringList &names) {
	clear_to_empty();
	m_asked = asked::account;

	m_heading->setText("<b>Choose a passkey</b>");
	m_detail->setText(QString("Which passkey do you want to use for %1?")
	                      .arg(site_or_this_site(m_relying_party)));

	auto *rows = qobject_cast<QVBoxLayout *>(m_accounts->layout());
	int id = 0;
	for (const QString &name : names) {
		auto *choice = new QRadioButton(name, m_accounts);
		choice->setObjectName(QString("webauth_account_%1").arg(id));
		rows->insertWidget(rows->count() - 1, choice);
		m_account_group->addButton(choice, id);
		++id;
	}
	m_accounts_area->setVisible(true);

	// **Nothing selected to begin with**, for the reason `cert_dialog` gives
	// for the same choice: a preselected identity next to a default button is
	// one keystroke away from identifying somebody who was reading the question
	// rather than answering it. Choosing which passkey to present is exactly
	// that kind of decision.
	m_accept->setText("Use passkey");
	m_accept->setVisible(true);
	m_accept->setEnabled(false);
	refresh_accept();
}

void webauth_dialog::ask_for_pin(const pin_prompt &prompt) {
	clear_to_empty();
	m_asked = asked::pin;
	m_min_pin = prompt.min_length > 0 ? prompt.min_length : 0;

	const bool new_pin = prompt.reason != pin_reason::challenge;
	switch (prompt.reason) {
		case pin_reason::challenge:
			m_heading->setText("<b>Enter your security key's PIN</b>");
			m_detail->setText(QString("The key is checking that it is you before "
			                           "it signs you in to %1.")
			                      .arg(site_or_this_site(m_relying_party)));
			break;
		case pin_reason::set:
			m_heading->setText("<b>Set a PIN for your security key</b>");
			m_detail->setText("The key has no PIN yet and needs one before it "
			                   "can be used. It protects every passkey stored "
			                   "on the key.");
			break;
		case pin_reason::change:
			m_heading->setText("<b>Change your security key's PIN</b>");
			m_detail->setText("The key will not go on until its PIN is changed.");
			break;
	}

	// The confirmation exists only where a wrong entry is unrecoverable. Typing
	// an existing PIN wrongly costs one try and says so; typing a *new* one
	// wrongly sets the key to a PIN nobody knows, and the only way back from
	// that is a reset that erases the passkeys.
	m_confirming = new_pin;
	m_confirm_label->setVisible(new_pin);
	m_confirm->setVisible(new_pin);

	QStringList trouble;
	const QString said = pin_error_text(prompt.error);
	if (!said.isEmpty())
		trouble << said;
	if (m_min_pin > 0)
		trouble << QString("At least %1 characters.").arg(m_min_pin);
	// **Zero remaining is a real answer**, so it is compared against a negative
	// rather than tested for truth: the next wrong PIN locks the key, and that
	// is the one case where saying nothing is worst.
	if (prompt.remaining_attempts >= 0)
		trouble << (prompt.remaining_attempts == 1
		              ? QStringLiteral("1 try left before the key locks itself.")
		              : QString("%1 tries left before the key locks itself.")
		                    .arg(prompt.remaining_attempts));
	m_pin_trouble->setText(trouble.join(' '));

	m_pin_box->setVisible(true);
	m_accept->setText("Next");
	m_accept->setVisible(true);
	m_accept->setEnabled(false);
	m_pin->setFocus();
	refresh_accept();
}

void webauth_dialog::ask_for_touch() {
	clear_to_empty();
	m_asked = asked::touch;
	m_heading->setText(QString("<b>Use your security key with %1</b>")
	                       .arg(site_or_this_site(m_relying_party)));
	m_detail->setText("Touch your security key again to finish.");
	// No accept button: there is nothing on screen to accept. The next thing
	// that happens comes from the key, or from Cancel.
}

void webauth_dialog::report_failure(failure why) {
	clear_to_empty();
	m_asked = asked::trouble;
	m_heading->setText("<b>Signing in did not work</b>");
	m_detail->setText(failure_text(why, m_relying_party));
	const bool again = retryable(why);
	m_retry->setVisible(again);
	// "Close" rather than "Cancel": there is nothing left to cancel, and a
	// Cancel button on a window reporting a failure reads as though pressing it
	// might undo something.
	m_cancel->setText("Close");
	// The flag, not `m_retry->isVisible()`, for the reason the confirmation
	// field is tracked by one: a child of an unshown window reports itself
	// invisible whatever was asked of it.
	if (again)
		m_retry->setFocus();
	else
		m_cancel->setFocus();
}
