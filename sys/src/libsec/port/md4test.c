#include "os.h"
#include <mp.h>
#include <libsec.h>

char *tests[] = {
	"",
	"a",
	"abc",
	"message digest",
	"abcdefghijklmnopqrstuvwxyz",
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
	"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
	"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhi"
		"jklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	0
};

char *results[] = {
	"31d6cfe0d16ae931b73c59d7e0c089c0",
	"bde52cb31de33e46245e05fbdbd6fb24",
	"a448017aaf21d8525fc10ae87aa6729d",
	"d9130a8164549fe818874806e1c7014bb",
	"d79e1c308aa5bbcdeea8ed63df412da9",
	"043f8582f241db351ce627e153e7f0e4",
	"e33b4ddc9c38f2199c3e7b164fcc0536",
	"4691a9ec81b1a6bd1ab8557240b245c5",
	"2102d1d94bd58ebf5aa25c305bb783ad",
	"ea45093e86af8ebcb2b726219a751c95",
	"6bbc5782b7ea7d524e2ae912f6692542",
	"cf00bbb9ae1602d265810c8e18c272aa",
};

void
main(void)
{
	char **pp, **pp2;
	uchar *p;
	int i;
	uchar digest[MD4dlen], b[MD4dlen];
	DigestState st;

	for(pp = tests, pp2 = results; *pp; pp++, pp2++){
		p = (uchar*)*pp;
		md4(p, strlen(*pp), digest, 0);
		dec16(b, sizeof(b), *pp2, MD5dlen * 2);
		if(memcmp(b, digest, MD5dlen) != 0)
			print("FAIL: %s\n", *pp);
		for(i = 0; i < MD4dlen; i++)
			print("%2.2ux", digest[i]);
		print("\n");
	}

	/* test getting digest */
	md4((uchar*)tests[1], strlen(tests[1]), nil, &st);
	md4((uchar*)tests[2], strlen(tests[2]), nil, &st);
	md4(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[9], MD4dlen * 2);
	if(memcmp(b, digest, MD4dlen) != 0)
		print("FAIL: get digest\n");
	for(i = 0; i < MD4dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test add multiple messages and get digest again */
	md4((uchar*)tests[3], strlen(tests[3]), nil, &st);
	md4((uchar*)tests[4], strlen(tests[4]), nil, &st);
	md4(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[10], MD4dlen * 2);
	if(memcmp(b, digest, MD4dlen) != 0)
		print("FAIL: test add multiple messages and get digest\n");
	for(i = 0; i < MD4dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test last call */
	md4((uchar*)tests[5], strlen(tests[5]), digest, &st);
	dec16(b, sizeof(b), results[11], MD4dlen* 2);
	if(memcmp(b, digest, MD4dlen) != 0)
		print("FAIL: last call\n");
	for(i = 0; i < MD4dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");
}
