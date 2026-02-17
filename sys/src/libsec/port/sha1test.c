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
	"da39a3ee5e6b4b0d3255bfef95601890afd80709",
	"86f7e437faa5a7fce15d1ddcb9eaeaea377667b8",
	"a9993e364706816aba3e25717850c26c9cd0d89d",
	"c12252ceda8be8994d5fa0290a47231c1d16aae3",
	"32d10c7b8cf96570ca04ce37f2a19d84240d3a89",
	"761c457bf73b14d27e9e9265c46f4b4dda11f940",
	"50abf5706a150990a08b2c5ea40fa0e585554732",
	"84983e441c3bd26ebaae4aa1f95129e5e54670f1",
	"a49b2446a02c645bf419f995b67091253a04a259",
	"84bb906a0633227fc09c15558cda30058d7fb41a",
	"5f648e1bcd2864ea27222f736ac6e3734b87ee09",
	"23e6c4e53ae2d6f0d4908a097fab54cf75f29158",
};

void
main(void)
{
	char **pp, **pp2;
	uchar *p;
	int i;
	uchar digest[SHA1dlen], b[SHA1dlen];
	DigestState st;

	for(pp = tests, pp2 = results; *pp; pp++, pp2++){
		p = (uchar*)*pp;
		sha1(p, strlen(*pp), digest, 0);
		dec16(b, sizeof(b), *pp2, SHA1dlen * 2);
		if(memcmp(b, digest, SHA1dlen) != 0)
			print("FAIL: %s\n", *pp);
		for(i = 0; i < SHA1dlen; i++)
			print("%2.2ux", digest[i]);
		print("\n");
	}

	/* test getting digest */
	sha1((uchar*)tests[1], strlen(tests[1]), nil, &st);
	sha1((uchar*)tests[2], strlen(tests[2]), nil, &st);
	sha1(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[9], SHA1dlen * 2);
	if(memcmp(b, digest, SHA1dlen) != 0)
		print("FAIL: get digest\n");
	for(i = 0; i < SHA1dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test add multiple messages and get digest again */
	sha1((uchar*)tests[3], strlen(tests[3]), nil, &st);
	sha1((uchar*)tests[4], strlen(tests[4]), nil, &st);
	sha1(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[10], SHA1dlen * 2);
	if(memcmp(b, digest, SHA1dlen) != 0)
		print("FAIL: test add multiple messages and get digest\n");
	for(i = 0; i < SHA1dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test last call */
	sha1((uchar*)tests[5], strlen(tests[5]), digest, &st);
	dec16(b, sizeof(b), results[11], SHA1dlen* 2);
	if(memcmp(b, digest, SHA1dlen != 0))
		print("FAIL: last call\n");
	for(i = 0; i < SHA1dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");
}
