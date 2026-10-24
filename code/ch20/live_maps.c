// SPDX-License-Identifier: MIT
/*
 * live_maps.c - compare the teaching maps with real BPF maps (Exercise 20.5).
 *
 *   make live          (as root, on Linux 5.16 or later)
 *
 * Uses the bpf() system call directly, without libbpf, and needs no BPF
 * program: maps can be created, updated and looked up from user space.
 * It checks three things:
 *   1. a preallocated hash map refuses a new key when full but accepts an
 *      update of an existing key (Section 20.3);
 *   2. the kernel's LPM trie and lpm.c give the same answer for random
 *      lookups after the same random updates and deletes, and iterate in
 *      the same order (Section 20.6);
 *   3. with BPF_F_ZERO_SEED, the kernel's Bloom filter and bloom.c let
 *      exactly the same non-members through (Section 20.7).
 */
#include <linux/bpf.h>	/* first: it defines BPF_ANY etc. as enums */

#include "bloom.h"
#include "htab.h"
#include "lpm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)			/* xorshift64 */
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static int map_create(int type, int key, int value, int max, int flags,
		      uint64_t extra)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_type = type;
	a.key_size = key;
	a.value_size = value;
	a.max_entries = max;
	a.map_flags = flags;
	a.map_extra = extra;
	return syscall(SYS_bpf, BPF_MAP_CREATE, &a, sizeof(a));
}

static int map_op(int cmd, int fd, const void *key, void *value,
		  uint64_t flags)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_fd = fd;
	a.key = (uintptr_t)key;
	a.value = (uintptr_t)value;
	a.flags = flags;
	return syscall(SYS_bpf, cmd, &a, sizeof(a)) ? -errno : 0;
}

static int next_key(int fd, const void *key, void *next)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_fd = fd;
	a.key = (uintptr_t)key;
	a.next_key = (uintptr_t)next;
	return syscall(SYS_bpf, BPF_MAP_GET_NEXT_KEY, &a, sizeof(a)) ?
	       -errno : 0;
}

static void hash_map(void)
{
	int fd = map_create(BPF_MAP_TYPE_HASH, 4, 8, 8, 0, 0);
	struct htab *h = htab_create(4, 8, 8, 0);
	uint64_t v = 1;
	uint32_t key;

	if (fd < 0 || !h) {
		perror("hash map");
		exit(1);
	}
	for (key = 0; key < 8; key++) {
		map_op(BPF_MAP_UPDATE_ELEM, fd, &key, &v, BPF_ANY);
		htab_update(h, &key, &v, BPF_ANY);
	}
	printf("hash map, max_entries 8: 9th key: kernel %d, teaching %d; ",
	       map_op(BPF_MAP_UPDATE_ELEM, fd, &key, &v, BPF_ANY),
	       htab_update(h, &key, &v, BPF_ANY));
	key = 3;
	printf("update key 3: kernel %d, teaching %d\n",
	       map_op(BPF_MAP_UPDATE_ELEM, fd, &key, &v, BPF_EXIST),
	       htab_update(h, &key, &v, BPF_EXIST));
	htab_destroy(h);
	close(fd);
}

struct kkey {				/* struct bpf_lpm_trie_key_u8 + 4 */
	uint32_t prefixlen;
	uint8_t data[4];
};

static void random_prefix(struct lpm_key *k, struct kkey *kk)
{
	static const uint8_t first[] = { 10, 172, 192, 100 };

	memset(k, 0, sizeof(*k));
	k->data[0] = first[rnd() % 4];
	k->data[1] = rnd() % 4;
	k->data[2] = rnd() % 16;
	k->data[3] = rnd();
	k->prefixlen = rnd() % 33;
	kk->prefixlen = k->prefixlen;
	memcpy(kk->data, k->data, 4);
}

static void lpm_trie(void)
{
	int fd = map_create(BPF_MAP_TYPE_LPM_TRIE, sizeof(struct kkey), 4,
			    500, BPF_F_NO_PREALLOC, 0);
	struct lpm_trie t;
	struct lpm_key k;
	struct kkey kk, kprev;
	long agree = 0, same_err = 0, n;

	if (fd < 0) {
		perror("LPM trie");
		exit(1);
	}
	lpm_init(&t, 4, 500);
	for (int i = 0; i < 20000; i++) {
		uint32_t value = rnd();
		int flags = rnd() % 3, a, b;

		random_prefix(&k, &kk);
		if (rnd() % 4) {
			a = map_op(BPF_MAP_UPDATE_ELEM, fd, &kk, &value, flags);
			b = lpm_update(&t, &k, value, flags);
		} else {
			a = map_op(BPF_MAP_DELETE_ELEM, fd, &kk, NULL, 0);
			b = lpm_delete(&t, &k);
		}
		same_err += a == b;
	}
	for (n = 0; n < 100000; n++) {
		uint32_t kv = 0, *tv;
		int a;

		random_prefix(&k, &kk);
		k.prefixlen = kk.prefixlen = 32;
		a = map_op(BPF_MAP_LOOKUP_ELEM, fd, &kk, &kv, 0);
		tv = lpm_lookup(&t, &k);
		agree += (a == -ENOENT && !tv) || (a == 0 && tv && *tv == kv);
	}
	printf("LPM trie: %ld of 20000 updates/deletes returned the same, "
	       "%ld of %ld lookups agree\n", same_err, agree, n);

	/* Walk both in their own order and compare key by key. */
	long steps = 0, order = 0;
	struct lpm_key tk, tprev;
	int a = next_key(fd, NULL, &kk), b = lpm_get_next_key(&t, NULL, &tk);

	while (a == 0 && b == 0) {
		steps++;
		order += kk.prefixlen == tk.prefixlen &&
			 !memcmp(kk.data, tk.data, 4);
		kprev = kk;
		tprev = tk;
		a = next_key(fd, &kprev, &kk);
		b = lpm_get_next_key(&t, &tprev, &tk);
	}
	printf("LPM trie: %zu entries; iteration: %ld of %ld keys in the "
	       "same position, both ended: %s\n", t.n_entries, order, steps,
	       a == -ENOENT && b == -ENOENT ? "yes" : "no");
	lpm_destroy(&t);
	close(fd);
}

static void bloom(uint32_t n, uint32_t k)
{
	int fd = map_create(BPF_MAP_TYPE_BLOOM_FILTER, 0, 4, n,
			    BPF_F_ZERO_SEED, k);
	struct bloom *b = bloom_create(4, n, k, 0);
	long kpass = 0, tpass = 0, differ = 0;

	if (fd < 0 || !b) {
		perror("Bloom filter (BPF_F_ZERO_SEED needs CAP_SYS_ADMIN)");
		exit(1);
	}
	for (uint32_t v = 0; v < n; v++) {
		map_op(BPF_MAP_UPDATE_ELEM, fd, NULL, &v, BPF_ANY);
		bloom_push(b, &v);
	}
	for (uint32_t v = n; v < n + 1000000; v++) {
		int kp = map_op(BPF_MAP_LOOKUP_ELEM, fd, NULL, &v, 0) == 0;
		int tp = bloom_peek(b, &v) == 0;

		kpass += kp;
		tpass += tp;
		differ += kp != tp;
	}
	printf("Bloom n = %u, k = %u: kernel passes %ld, teaching %ld of "
	       "1000000; answers differ for %ld\n", n, k, kpass, tpass, differ);
	bloom_destroy(b);
	close(fd);
}

int main(void)
{
	hash_map();
	lpm_trie();
	bloom(10000, 5);
	bloom(15000, 3);
	return 0;
}
