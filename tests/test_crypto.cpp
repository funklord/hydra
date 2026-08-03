// The crypto shim under the KeePassXC bridge (architecture doc §13.1).
//
// There is no cryptography of our own here and there should never be: this wraps
// libsodium's `crypto_box` and nothing else. What is worth testing is not the
// cipher — libsodium's own suite does that far better — but **the shim's edges**,
// where a mistake would be ours: whether a wrong key is refused rather than
// accepted, whether a tampered message is rejected rather than returned, and
// whether a bad size fails cleanly rather than reading past the end of a buffer.
//
// A shim that answers "false" on every failure path is the whole requirement. A
// shim that answers "true" with garbage would hand the bridge a forged reply and
// look exactly like success.
//
// This file compiles and runs either way: without libsodium the contract is that
// everything fails and `available()` says so, which is also worth pinning, since
// that build is the one where the password manager must report itself unusable
// rather than pretend.
#include "box_crypto.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	if (!box_crypto::available()) {
		section("built without libsodium: everything must fail, and say so");
		QByteArray a, b, out;
		check(!box_crypto::keypair(&a, &b), "no keypair");
		check(box_crypto::random_nonce().isEmpty(), "no nonce");
		check(box_crypto::random_bytes(32).isEmpty(), "no random bytes");
		check(!box_crypto::seal("x", "n", "p", "s", &out), "no sealing");
		check(!box_crypto::open("x", "n", "p", "s", &out), "no opening");
		check(out.isEmpty(), "and nothing is written to the output on failure");
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return g_fail == 0 ? 0 : 1;
	}

	section("keys and nonces");
	QByteArray a_pub, a_sec, b_pub, b_sec;
	{
		check(box_crypto::keypair(&a_pub, &a_sec), "a keypair is made");
		check(box_crypto::keypair(&b_pub, &b_sec), "and a second one");
		check(a_pub.size() == 32 && a_sec.size() == 32,
		      QString("X25519 keys are 32 bytes (%1, %2)").arg(a_pub.size()).arg(a_sec.size()));
		check(a_pub != b_pub && a_sec != b_sec,
		      "two keypairs differ — not a constant dressed as a key");
		check(a_pub != a_sec, "and the public half is not the secret half");

		const QByteArray n1 = box_crypto::random_nonce();
		const QByteArray n2 = box_crypto::random_nonce();
		check(n1.size() == 24, QString("a nonce is 24 bytes (%1)").arg(n1.size()));
		check(n1 != n2, "and two of them differ");
		check(box_crypto::random_bytes(7).size() == 7, "random_bytes gives what is asked");
		check(box_crypto::random_bytes(0).isEmpty(), "zero bytes is empty, not a crash");
		check(box_crypto::random_bytes(-1).isEmpty(), "and so is a negative count");
	}

	section("a message from A to B, and back");
	{
		const QByteArray nonce = box_crypto::random_nonce();
		const QByteArray plain = "{\"action\":\"get-logins\"}";
		QByteArray cipher, opened;

		check(box_crypto::seal(plain, nonce, b_pub, a_sec, &cipher),
		      "A seals to B's public key with A's secret");
		check(cipher.size() == plain.size() + 16,
		      QString("the ciphertext carries a 16-byte tag (%1 vs %2)")
		          .arg(cipher.size()).arg(plain.size()));
		check(cipher != plain, "and is not the plaintext");

		check(box_crypto::open(cipher, nonce, a_pub, b_sec, &opened),
		      "B opens it with A's public key and B's secret");
		check(opened == plain, "and gets exactly what was sent");
	}

	section("an empty message is still a message");
	{
		const QByteArray nonce = box_crypto::random_nonce();
		QByteArray cipher, opened;
		check(box_crypto::seal(QByteArray(), nonce, b_pub, a_sec, &cipher),
		      "an empty plaintext seals");
		check(cipher.size() == 16, "to exactly the tag");
		check(box_crypto::open(cipher, nonce, a_pub, b_sec, &opened) && opened.isEmpty(),
		      "and opens back to empty rather than failing");
	}

	section("what must not open");
	{
		const QByteArray nonce = box_crypto::random_nonce();
		const QByteArray plain = "secret";
		QByteArray cipher, out;
		box_crypto::seal(plain, nonce, b_pub, a_sec, &cipher);

		QByteArray tampered = cipher;
		tampered[0] = static_cast<char>(tampered[0] ^ 0x01);
		out.clear();
		check(!box_crypto::open(tampered, nonce, a_pub, b_sec, &out),
		      "one flipped bit is refused — this is the whole point of the tag");
		check(out.isEmpty(), "and nothing is handed back from the attempt");

		QByteArray truncated = cipher;
		truncated.chop(1);
		check(!box_crypto::open(truncated, nonce, a_pub, b_sec, &out),
		      "a truncated ciphertext is refused");

		const QByteArray other_nonce = box_crypto::random_nonce();
		check(!box_crypto::open(cipher, other_nonce, a_pub, b_sec, &out),
		      "the wrong nonce is refused");

		QByteArray c_pub, c_sec;
		box_crypto::keypair(&c_pub, &c_sec);
		check(!box_crypto::open(cipher, nonce, c_pub, b_sec, &out),
		      "a third party's public key does not open it");
		check(!box_crypto::open(cipher, nonce, a_pub, c_sec, &out),
		      "and neither does a third party's secret");
	}

	section("sizes are checked before libsodium is handed a buffer");
	{
		// These are the calls where a missing length check is not a wrong answer
		// but a read past the end of an array. The bridge builds its arguments
		// from what arrives on a socket, so "the caller would never" is not an
		// argument available here.
		const QByteArray nonce = box_crypto::random_nonce();
		QByteArray out;
		check(!box_crypto::seal("x", QByteArray(23, 'n'), b_pub, a_sec, &out),
		      "a nonce one byte short is refused");
		check(!box_crypto::seal("x", nonce, QByteArray(31, 'p'), a_sec, &out),
		      "a short public key is refused");
		check(!box_crypto::seal("x", nonce, b_pub, QByteArray(31, 's'), &out),
		      "a short secret key is refused");
		check(!box_crypto::seal("x", nonce, QByteArray(), QByteArray(), &out),
		      "and empty keys are refused rather than treated as zeros");

		check(!box_crypto::open(QByteArray(15, 'c'), nonce, a_pub, b_sec, &out),
		      "a ciphertext shorter than the tag cannot be authentic");
		check(!box_crypto::open(QByteArray(), nonce, a_pub, b_sec, &out),
		      "and an empty one is not either");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
