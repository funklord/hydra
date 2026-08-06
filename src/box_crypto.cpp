// SPDX-License-Identifier: GPL-3.0-or-later
#include "box_crypto.h"

/// @pkg_optional libsodium defines HYDRA_HAVE_SODIUM
#ifdef HYDRA_HAVE_SODIUM
#include <sodium.h>
#endif

namespace box_crypto {

#ifdef HYDRA_HAVE_SODIUM

namespace {

bool ensure_init() {
	static const bool ok = (sodium_init() >= 0);
	return ok;
}

}  // namespace

bool available() { return ensure_init(); }

bool keypair(QByteArray *public_key, QByteArray *secret_key) {
	if (!ensure_init())
		return false;
	QByteArray pk(crypto_box_PUBLICKEYBYTES, Qt::Uninitialized);
	QByteArray sk(crypto_box_SECRETKEYBYTES, Qt::Uninitialized);
	if (crypto_box_keypair(reinterpret_cast<unsigned char *>(pk.data()),
		                     reinterpret_cast<unsigned char *>(sk.data())) != 0)
		return false;
	*public_key = pk;
	*secret_key = sk;
	return true;
}

QByteArray random_nonce() {
	return random_bytes(crypto_box_NONCEBYTES);
}

QByteArray random_bytes(int n) {
	if (!ensure_init() || n <= 0)
		return {};
	QByteArray out(n, Qt::Uninitialized);
	randombytes_buf(out.data(), static_cast<size_t>(n));
	return out;
}

bool seal(const QByteArray &plain, const QByteArray &nonce,
	        const QByteArray &their_public, const QByteArray &our_secret,
	        QByteArray *out) {
	if (!ensure_init() || nonce.size() != crypto_box_NONCEBYTES ||
		  their_public.size() != crypto_box_PUBLICKEYBYTES ||
		  our_secret.size() != crypto_box_SECRETKEYBYTES)
		return false;
	QByteArray cipher(plain.size() + crypto_box_MACBYTES, Qt::Uninitialized);
	if (crypto_box_easy(reinterpret_cast<unsigned char *>(cipher.data()),
		                  reinterpret_cast<const unsigned char *>(plain.constData()),
		                  static_cast<unsigned long long>(plain.size()),
		                  reinterpret_cast<const unsigned char *>(nonce.constData()),
		                  reinterpret_cast<const unsigned char *>(their_public.constData()),
		                  reinterpret_cast<const unsigned char *>(our_secret.constData())) != 0)
		return false;
	*out = cipher;
	return true;
}

bool open(const QByteArray &cipher, const QByteArray &nonce,
	        const QByteArray &their_public, const QByteArray &our_secret,
	        QByteArray *out) {
	if (!ensure_init() || cipher.size() < crypto_box_MACBYTES ||
		  nonce.size() != crypto_box_NONCEBYTES ||
		  their_public.size() != crypto_box_PUBLICKEYBYTES ||
		  our_secret.size() != crypto_box_SECRETKEYBYTES)
		return false;
	QByteArray plain(cipher.size() - crypto_box_MACBYTES, Qt::Uninitialized);
	if (crypto_box_open_easy(reinterpret_cast<unsigned char *>(plain.data()),
		                       reinterpret_cast<const unsigned char *>(cipher.constData()),
		                       static_cast<unsigned long long>(cipher.size()),
		                       reinterpret_cast<const unsigned char *>(nonce.constData()),
		                       reinterpret_cast<const unsigned char *>(their_public.constData()),
		                       reinterpret_cast<const unsigned char *>(our_secret.constData())) != 0)
		return false;   // authentication failed — tampered or wrong key
	*out = plain;
	return true;
}

#else   // built without libsodium

bool available() { return false; }
bool keypair(QByteArray *, QByteArray *) { return false; }
QByteArray random_nonce() { return {}; }
QByteArray random_bytes(int) { return {}; }
bool seal(const QByteArray &, const QByteArray &, const QByteArray &,
	        const QByteArray &, QByteArray *) { return false; }
bool open(const QByteArray &, const QByteArray &, const QByteArray &,
	        const QByteArray &, QByteArray *) { return false; }

#endif

}  // namespace box_crypto
