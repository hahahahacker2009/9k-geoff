#include <u.h>
#include <libc.h>
#include "libsec.h"

char *tests[] = {
	"",
	"a",
	"abc",
	"message digest",
	"abcdefghijklmnopqrstuvwxyz",
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
	"123456789012345678901234567890123456789012345678901234567890"
		"12345678901234567890",
	"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhi"
		"jklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	0
};

char *sha224[] = {
	"d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f",
	"abd37534c7d9a2efb9465de931cd7055ffdb8879563ae98078d6d6d5",
	"23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
	"2cb21c83ae2f004de7e81c3c7019cbcb65b71ab656b22d6d0c39b8eb",
	"45a5f72c39c5cff2522eb3429799e49e5f44b356ef926bcf390dccc2",
	"bff72b4fcb7d75e5632900ac5f90d219e05e97a7bde72e740db393d9",
	"b50aecbe4e9bb0b57bc5f3ae760a8e01db24f203fb3cdcd13148046e",
	"75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525",
	"c97ca9a559850ce97a04a96def6d99a9e0e0e2ab14e6b8df265fc0b3",
	"fb5299479ff16c68a36a77df33dec5833a3620c75c45006a626988d5",
	"db8faa0b41103e477eddb96c2b807e57dda5829f66d9af6755f8b61e",
	"e8c7feca2b388baaa9cfc3b39bbdfac000dd4d74a0f7a8aab04f8946",
};

char *sha256[] = {
	"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	"f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650",
	"71c480df93d6ae2f1efad1447c66c9525e316218cf51fc8d9ed832f2daf18b73",
	"db4bfcbd4da0cd85a60c3c37d3fbd8805c77f15fc6b1fdfe614ee0a7c8fdb4c0",
	"f371bc4a311f2b009eef952dd83ca80e2b60026c8e935592d0f9c308453c813e",
	"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
	"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
	"223189358b15aac5609ec6d35ccfa7df93b8f2507344c03445351586b0459228",
	"b7d8e298b5bd9fb9353c4e64ae5cfd7302a25fe94f0e5ed2f185c0c24cf501a7",
	"1c872b2e76ed361dd5ebce44e7211a2124e5011e5edf5b30f4dc741ac5cb8cb0",
};

char *sha384[] = {
	"38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
	"54a59b9f22b0b80880d8427e548b7c23abd873486e1f035dce9cd697e85175033caa88e6d57bc35efae0b5afd3145f31",
	"cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
	"473ed35167ec1f5d8e550368a3db39be54639f828868e9454c239fc8b52e3c61dbd0d8b4de1390c256dcbb5d5fd99cd5",
	"feb67349df3db6f5924815d6c3dc133f091809213731fe5c7b5f4999e463479ff2877f5f2936fa63bb43784b12f3ebb4",
	"1761336e3f7cbfe51deb137f026f89e01a448e3b1fafa64039c1464ee8732f11a5341a6f41e0c202294736ed64db1a84",
	"b12932b0627d1c060942f5447764155655bd4da0c9afa6dd9b9ef53129af1b8fb0195996d2de9ca0df9d821ffee67026",
	"3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b",
	"09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039",
	"6a534d2b4aa6f0f2d00a9d0f62880303ad3d0ac3e5c0252b323e6897269f4134da523e0c4b3af099b1421dcb844fc1fe",
	"43ab79841951ce0591ab68c88a1536e83bbbe23f1eb18c1478c4a33e7478f46ca6357eb9fa1770a8564eb1ddfaf68641",
	"8f2d76ffd805526246c8ff7af1f6293b2454a804fd94afb7e46984433a27f025dec09df035405b0a374f0bf98cc6f45e",
};

char *sha512[] = {
	"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
	"1f40fc92da241694750979ee6cf582f2d5d7d28e18335de05abc54d0560e0f5302860c652bf08d560252aa5e74210546f369fbbbce8c12cfc7957b2652fe9a75",
	"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
	"107dbf389d9e9f71a3a95f6c055b9251bc5268c2be16d6c13492ea45b0199f3309e16455ab1e96118e8a905d5597b72038ddb372a89826046de66687bb420e7c",
	"4dbff86cc2ca1bae1e16468a05cb9881c97f1753bce3619034898faa1aabe429955a1bf8ec483d7421fe3c1646613a59ed5441fb0f321389f77f48a879c7b1f1",
	"1e07be23c26a86ea37ea810c8ec7809352515a970e9253c26f536cfc7a9996c45c8370583e0a78fa4a90041d71a4ceab7423f19c71b9d5a3e01249f0bebd5894",
	"72ec1ef1124a45b047e8b7c75a932195135bb61de24ec0d1914042246e0aec3a2354e093d76f3048b456764346900cb130d2a4fd5dd16abb5e30bcb850dee843",
	"204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445",
	"8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
	"f545146094761d524980e1892ec1ab6be1621aff89f1e529cb414b50c7c90e44ef06173d93ca6523fec475522be6bfae0bda9a0ee3deef70ca8317e23cb360d0",
	"1cc559f31bc504a14e66946799dbbc2e28ed0646d5f802247cd34f36826fced2435a6f57c837f2db05f4ae45f9bf14a7afcbd7ff36365064537b76349613a510",
	"6f0fc6be9ae9ca1f45d88b5cda3121311b4a91e3cae8641f67e9757dd590a40847d409f3ba9b99192e8780b8c6e5e7376fa7489eae8cb194895ab640b9c4554f",
};

void
runtests(char *name, DigestState*(*x)(uchar*, ulong, uchar*, DigestState*), int xlen, char** results)
{
	int i;
	char **pp, **pp2;
	uchar *p;
	uchar digest[SHA2_512dlen], b[SHA2_512dlen];
	DigestState st;

	print("\n%s tests:\n", name);
	for(pp = tests, pp2 = results; *pp; pp++, pp2++){
		p = (uchar*)*pp;
		x(p, strlen(*pp), digest, 0);
		dec16(b, sizeof(b), *pp2, xlen * 2);
		if(memcmp(b, digest, xlen) != 0)
			print("%s: FAIL: %s\n", name, *pp);
		for(i = 0; i < xlen; i++)
			print("%2.2ux", digest[i]);
		print("\n");
	}

	memset(&st, 0, sizeof(st));

	/* test getting digest */
	x((uchar*)tests[1], strlen(tests[1]), nil, &st);
	x((uchar*)tests[2], strlen(tests[2]), nil, &st);
	x(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[9], xlen * 2);
	if(memcmp(b, digest, xlen) != 0)
		print("%s: FAIL: get digest\n", name);
	for(i = 0; i < xlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test add multiple messages and get digest again */
	x((uchar*)tests[3], strlen(tests[3]), nil, &st);
	x((uchar*)tests[4], strlen(tests[4]), nil, &st);
	x(nil, 0, digest, &st);
	dec16(b, sizeof(b), results[10], xlen * 2);
	if(memcmp(b, digest, xlen) != 0)
		print("%s: FAIL: test add multiple messages and get digest\n", name);
	for(i = 0; i < xlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

	/* test last call */
	x((uchar*)tests[5], strlen(tests[5]), digest, &st);
	dec16(b, sizeof(b), results[11], xlen * 2);
	if(memcmp(b, digest, xlen) != 0)
		print("%s: FAIL: last call\n", name);
	for(i = 0; i < xlen; i++)
		print("%2.2ux", digest[i]);
	print("\n");

}

void
main(void)
{
	runtests("sha224", sha2_224, SHA2_224dlen, sha224);
	runtests("sha256", sha2_256, SHA2_256dlen, sha256);
	runtests("sha384", sha2_384, SHA2_384dlen, sha384);
	runtests("sha512", sha2_512, SHA2_512dlen, sha512);
}
