// SPDX-License-Identifier: MIT
/*
 * rsa.h - textbook RSA on top of bn.h: the public operation, the
 * private operation with the Chinese Remainder Theorem as in the
 * kernel's crypto/rsa.c (RFC 3447, RSADP), and a deterministic toy key
 * generator for the tests.
 *
 * Left out: padding (PKCS#1, PSS, OAEP), ASN.1 key parsing, the
 * kernel's key-length and FIPS exponent checks, and any defence against
 * timing or fault attacks.  The kernel never generates RSA keys; the
 * generator here (trial division, then a Fermat test) exists only to
 * produce test keys and is not suitable for real keys.
 */
#ifndef RSA_H
#define RSA_H

#include "bn.h"

struct rsa_key {
	struct bn n, e, d;		/* public modulus and exponent; private d */
	struct bn p, q;			/* n = p q */
	struct bn dp, dq, qinv;		/* d mod (p-1), d mod (q-1), q^-1 mod p */
};

int rsa_enc(const struct rsa_key *k, struct bn *c, const struct bn *m);
int rsa_dec_direct(const struct rsa_key *k, struct bn *m, const struct bn *c);
int rsa_dec_crt(const struct rsa_key *k, struct bn *m, const struct bn *c);
void rsa_keygen(struct rsa_key *k, int bits, uint64_t (*rnd)(void));

#endif /* RSA_H */
