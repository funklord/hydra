// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QStringList>

class QButtonGroup;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QWidget;

// Signing in with a passkey: the whole conversation an authenticator can have,
// in one window.
//
// **Nothing handled this, and nothing could see that it was not handled.** A
// page calling `navigator.credentials.get()` puts the engine into a state
// machine that waits for a person -- choose an account, type the PIN, touch the
// key -- and with no window attached there is nobody for it to wait for. The
// sign-in does not fail and does not say why: it stalls until the request times
// out, which reads as a site that is broken. That is the same shape as the
// fullscreen and window-request gaps this seam already records, and it is the
// expensive one, because the workflow it breaks is signing in.
//
// **This file knows nothing about Qt WebEngine** (architecture doc sec 19.2).
// It is driven by plain calls and answers with plain signals, so the engine's
// request object never leaves the backend that owns it. That is not only the
// seam rule: `hydra.pro` drops the four `qtwebengine_*` files on Android and
// keeps everything else, so a dialog that named the engine would stop that
// build compiling.
//
// It is a state display rather than a wizard. The engine decides which question
// is being asked and when it stops being asked, so each `ask_*` call rebuilds
// the window for one state, and the caller drives them in whatever order the
// authenticator produces -- including backwards, which a mistyped PIN does.
class webauth_dialog : public QDialog {
	Q_OBJECT
public:
	// Why a PIN is wanted. `challenge` is the ordinary case: the key already
	// has one and is asking for it. The other two are setting one up, and get
	// a confirmation field, because a typo there becomes a PIN nobody knows.
	enum class pin_reason { challenge, set, change };

	// What was wrong with the previous PIN, where there was one.
	enum class pin_error { none, uv_locked, wrong_pin, too_short,
		                      invalid_characters, same_as_current };

	// Why the request stopped. **`unknown` is not padding.** The engine's list
	// of reasons has grown between releases, and a value this build has never
	// heard of still has to produce a sentence rather than an empty window.
	enum class failure { timeout, key_not_registered, key_already_registered,
		                    soft_pin_block, hard_pin_block,
		                    removed_during_pin_entry, no_resident_keys,
		                    no_user_verification, no_large_blob,
		                    no_common_algorithms, storage_full, consent_denied,
		                    cancelled_by_system, unknown };

	struct pin_prompt {
		pin_reason reason = pin_reason::challenge;
		pin_error  error  = pin_error::none;
		// Shortest PIN the key will take, in characters. Zero means the key did
		// not say, which is not "any length is fine" but "there is nothing to
		// check here" -- so the only rule left is that something was typed.
		int min_length = 0;
		// Tries left before the key locks itself. Negative means the key did
		// not say; **zero is a real answer and an alarming one**, so it must
		// not share a spelling with "unknown".
		int remaining_attempts = -1;
	};

	explicit webauth_dialog(const QString &relying_party,
	                         QWidget *parent = nullptr);

	// One call per state the engine reports. Each rebuilds the window: the
	// previous question is gone rather than stacked behind this one.
	void ask_for_account(const QStringList &names);
	void ask_for_pin(const pin_prompt &prompt);
	void ask_for_touch();
	void report_failure(failure why);

signals:
	void account_chosen(const QString &name);
	void pin_entered(const QString &pin);
	void retry_requested();

	// The person is finished with it: Cancel, Escape, or the window's close
	// button. **The caller has to pass this on to the engine**, because closing
	// a window withdraws nothing -- the request is still outstanding, and one
	// nobody withdraws holds the authenticator until it times out.
	void cancelled();

private:
	// Which question is on screen. The button box only knows it was clicked,
	// and the accept button means a different thing in each state.
	enum class asked { nothing, account, pin, touch, trouble };

	// Everything hidden and every per-question widget emptied, so a state
	// cannot inherit a leftover from the one before it. The account radio
	// buttons are the ones that matter: they are built per request, and a set
	// left standing would offer names from a sign-in that has already moved on.
	void clear_to_empty();

	// Whether the accept button has an argument yet. Called from every edit and
	// every toggle rather than validating on click, because a button that is
	// available and then refuses is a button that lied.
	void refresh_accept();

	QString m_relying_party;
	asked m_asked = asked::nothing;
	int m_min_pin = 0;
	// Whether the PIN has to be typed twice. **Kept as a flag rather than read
	// back from the widget**, because `isVisible()` is false for every child of
	// a window that has not been shown yet -- so asking the widget would skip
	// the confirmation check for exactly the first question, which is the one
	// that sets a new PIN.
	bool m_confirming = false;

	QLabel *m_heading = nullptr;
	QLabel *m_detail = nullptr;
	QScrollArea *m_accounts_area = nullptr;
	QWidget *m_accounts = nullptr;
	QButtonGroup *m_account_group = nullptr;
	QWidget *m_pin_box = nullptr;
	QLineEdit *m_pin = nullptr;
	QLabel *m_confirm_label = nullptr;
	QLineEdit *m_confirm = nullptr;
	QLabel *m_pin_trouble = nullptr;
	QDialogButtonBox *m_buttons = nullptr;
	QPushButton *m_accept = nullptr;
	QPushButton *m_retry = nullptr;
	QPushButton *m_cancel = nullptr;
};
