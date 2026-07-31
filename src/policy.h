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
// Parse a machine name; returns feature::count on failure.
feature     feature_from_name(const QString &name);

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
