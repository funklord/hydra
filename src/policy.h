// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QtGlobal>

// Per-site security features and the tri-state setting model (architecture
// doc §7.1). Each rule packs one 2-bit setting per feature into a quint64.
namespace policy {

enum class feature : int {
	javascript = 0,
	cookies,
	third_party_cookies,
	ads,
	popups,
	images,
	autoplay,
	geolocation,
	camera,
	microphone,
	notifications,
	referer,
	autofill,
	// The §11.5.1 helper tier, as two powers rather than one. Reading a
	// manifest the page already fetched is not comparable to reading the DOM of
	// a logged-in page, and a single "advanced extractor" switch would quietly
	// grant the second to get the first. Both default to block.
	extractor_fetch,
	extractor_dom,
	// "Do you want to accept cookies?" -- the consent banner itself, not the
	// cookies. Blocked means the banner is answered and dismissed for you;
	// allowed means it is left alone and you answer it yourself. It sits with
	// the other blocking options rather than under `cookies` because it is a
	// different question: what the *page* may put in front of you, not what it
	// may store.
	cookie_notices,
	count
};

// `unset` is the architecture doc's "Default" state — no rule expressed at
// this scope, so resolution falls through to the global default. Spelled
// `unset` because `default` is a keyword.
enum class setting : quint8 { unset = 0, allow = 1, block = 2 };

inline int feature_count() { return static_cast<int>(feature::count); }

// Stable machine name (JSON keys).
const char *feature_name(feature f);
// Human label (UI).
const char *feature_label(feature f);
// One line saying what the setting governs, shown under the label the way a
// browser's settings page does. It describes the *power*, not the state, so it
// reads correctly whichever way the setting is set — "where you are, when a page
// asks" is true whether that is allowed or blocked, while "sites cannot see
// where you are" would be a lie half the time.
const char *feature_help(feature f);
// Parse a machine name; returns feature::count on failure.
feature     feature_from_name(const QString &name);

// --- one line of settings, as the INI files write it ------------------------
//
// "javascript:block, cookies:allow" — the encoding shared by the policy file
// and the exported settings bundle. Shared because two encoders for one line
// drift, and the drift is invisible until a file written by one is read by the
// other.
QString  settings_to_line(quint64 bits);
quint64  settings_from_line(const QString &line);
const char *setting_word(setting s);
setting     setting_from_word(const QString &word);

// --- 2-bit packing into a quint64 -----------------------------------------
inline setting get_setting(quint64 bits, feature f) {
	const int shift = 2 * static_cast<int>(f);
	return static_cast<setting>((bits >> shift) & 0x3ULL);
}

inline quint64 with_setting(quint64 bits, feature f, setting s) {
	const int shift = 2 * static_cast<int>(f);
	bits &= ~(0x3ULL << shift);
	bits |= (static_cast<quint64>(s) & 0x3ULL) << shift;
	return bits;
}

}  // namespace policy
