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
	"d41d8cd98f00b204e9800998ecf8427e",
	"0cc175b9c0f1b6a831c399e269772661",
	"900150983cd24fb0d6963f7d28e17f72",
	"f96b697d7cb7938d525a2f31aaf161d0",
	"c3fcd3d76192e4007dfb496cca67e13b",
	"d174ab98d277d9f5a5611c2c9f419d9f",
	"57edf4a22be3c955ac49da2e2107b67a",
	"8215ef0796a20bcaaae116d3876c664a",
	"03dd8807a93175fb062dfb55dc7d359c",
	"e638f7d51818758264fa897a551e5511",
	"873c00d982f204ccdab885861de7bc8e",
	"954c3f7314357d821aba56216d5b45fc",
};

void
main(void)
{
	char **pp, **pp2;
	uchar *p;
	int i;
	uchar digest[MD5dlen], b[MD5dlen];
	DigestState st;

	for(pp = tests, pp2 = results; *pp; pp++, pp2++){
		p = (uchar*)*pp;
		md5(p, strlen(*pp), digest, 0);
		dec16(b, sizeof(b), *pp2, MD5dlen * 2);
		if(memcmp(b, digest, MD5dlen) != 0)
			print("FAIL: %s\n", *pp);
		for(i = 0; i < MD5dlen; i++)
			print("%2.2ux", digest[i]);
		print("\n");
	}

	/* test getting digest */
	md5((uchar*)tests[1], strlen(tests[1]), nil, &st);
	md5((uchar*)tests[2], strlen(tests[2]), nil, &st);
	md5(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[9], MD5dlen * 2);
	if(memcmp(b, digest, MD5dlen) != 0)
		print("FAIL: get digest\n");
	for(i = 0; i < MD5dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test add multiple messages and get digest again */
	md5((uchar*)tests[3], strlen(tests[3]), nil, &st);
	md5((uchar*)tests[4], strlen(tests[4]), nil, &st);
	md5(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[10], MD5dlen * 2);
	if(memcmp(b, digest, MD5dlen) != 0)
		print("FAIL: test add multiple messages and get digest\n");
	for(i = 0; i < MD5dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test last call */
	md5((uchar*)tests[5], strlen(tests[5]), digest, &st);
	dec16(b, sizeof(b), results[11], MD5dlen* 2);
	if(memcmp(b, digest, MD5dlen != 0))
		print("FAIL: last call\n");
	for(i = 0; i < MD5dlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");
}
