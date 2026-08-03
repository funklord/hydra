// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// Where the KeePassXC pairing is kept between runs (architecture doc §13.1,
// §13.3, §14).
//
// **What is stored is a capability, not a credential.** The association key is
// what proves to KeePassXC that this program is the one the user confirmed; it
// is not a password and it opens no vault on its own. But it is the whole of
// that proof, so it is the one thing this project keeps that has to be
// encrypted at rest — §14 says so, and the alternative is a key sitting beside
// the settings in a file anyone can read.
//
// **So it goes to the session's Secret Service** (libsecret / gnome-keyring,
// KWallet's bridge, or whatever the desktop provides), not to a file of ours.
// That is a deliberate refusal to invent storage: an "app-encrypted config"
// that the app can open unattended must keep its key on disk too, which is
// obfuscation wearing the word encryption. If there is no Secret Service, this
// says so and the pairing simply does not persist — the same
// "found, and the feature is on; absent, and it reports itself unavailable with
// no degraded mode" rule libsodium and libtorrent follow.
//
// The calls are synchronous. They talk to the session bus, so they can block if
// the keyring is locked and prompts — which is why nothing here runs at
// startup or in a hot path; every caller is a user-driven moment (pair,
// connect, forget). libsecret's async API wants a GLib main loop, and a Qt app
// is not guaranteed to be running one.
namespace credential_store {

// True when this build can store a pairing *and* a Secret Service answered.
// `unavailable_reason()` is empty exactly when this is true. The service is
// asked once and the answer cached: it is a blocking bus call.
bool available();
QString unavailable_reason();

// Both halves of the pairing, together, so they cannot get out of step. The id
// is the name the user gave the connection inside KeePassXC and is not itself
// secret; it travels with the key because half a pairing is not a pairing.
bool save(const QString &id, const QString &key_b64);
bool load(QString *id, QString *key_b64);
bool clear();

// Whether anything is stored, without unpacking it.
bool has_pairing();

// The stored form, split out so the encoding is testable with no keyring and no
// libsecret. Both fields are base64 in the blob, which is what makes a single
// space an unambiguous separator: neither can contain one.
QString encode_pair(const QString &id, const QString &key_b64);
bool decode_pair(const QString &blob, QString *id, QString *key_b64);

}  // namespace credential_store
