// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>

// The crypto the KeePassXC-Browser protocol needs: libsodium `crypto_box`
// (X25519 key exchange + XSalsa20-Poly1305 authenticated encryption), and
// nothing else (architecture doc §13.1).
//
// This is a thin shim, not an abstraction layer. It exists so the protocol and
// bridge compile whether or not libsodium is present: when it is absent every
// call fails and `available()` is false, so the password manager reports itself
// unusable instead of the whole application failing to build over an optional
// dependency.
//
// We implement no crypto of our own and never see the master password —
// KeePassXC holds the vault, the unlock, and everything derived from it (§13.3).
namespace crypto_box {

// False when Hydra was built without libsodium.
bool available();

// X25519 keypair. Both come back empty on failure.
bool keypair(QByteArray *public_key, QByteArray *secret_key);

// 24-byte nonce.
QByteArray random_nonce();

// Authenticated encryption to `their_public` from `our_secret`.
bool seal(const QByteArray &plain, const QByteArray &nonce,
          const QByteArray &their_public, const QByteArray &our_secret,
          QByteArray *out);

bool open(const QByteArray &cipher, const QByteArray &nonce,
          const QByteArray &their_public, const QByteArray &our_secret,
          QByteArray *out);

// Random bytes, used for the association id key.
QByteArray random_bytes(int n);

}  // namespace crypto_box
