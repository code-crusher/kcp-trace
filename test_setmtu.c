//=====================================================================
//
// test_setmtu.c - regression test for issue #433
//
// ikcp_setmtu() shrinks kcp->buffer, but segments already queued with
// the previous (larger) mss stay in snd_queue/snd_buf. On the next
// ikcp_flush() (invoked by ikcp_update), such a segment plus its 24
// byte header was memcpy'd past the end of the new smaller buffer ->
// heap corruption / AccessViolationException in ikcp_update
// (see https://github.com/skywind3000/kcp/issues/433).
//
// The fix: ikcp_setmtu never sizes the buffer below the largest size
// it ever had, so any segment queued earlier still fits.
//
// Run under -fsanitize=address to detect the past overflow:
//   gcc -I. test_setmtu.c ikcp.c -o test_setmtu -fsanitize=address
//
//=====================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ikcp.h"

static int output_cb(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)kcp; (void)user;
	return 0;
}

// minimal shape of the crash from issue #433: queue large segments,
// shrink the MTU, then call ikcp_update -> flush copies a stale
// oversized segment out of kcp->buffer
static int test_shrink_with_pending(void)
{
	ikcpcb *kcp = ikcp_create(0x11223344, NULL);
	if (kcp == NULL) return 1;
	ikcp_setoutput(kcp, output_cb);
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	{
		char data[3000];
		memset(data, 0x61, sizeof(data));
		ikcp_send(kcp, data, 3000);		// 1376 + 1376 + 248 fragments
		if (ikcp_setmtu(kcp, 400) != 0) {	// buffer would be 1272 bytes
			ikcp_release(kcp);
			return 1;
		}
		ikcp_update(kcp, 1000);			// crashed here before the fix
		ikcp_update(kcp, 1010);
	}
	ikcp_release(kcp);
	return 0;
}

// randomized send/setmtu interleavings in every kcp mode; the crash
// needs a grow -> send -> shrink cycle, which defeats a fix that only
// compares against the immediately previous mtu
static int test_random_interleave(void)
{
	unsigned seed;
	for (seed = 0; seed < 8; seed++) {
		int iter;
		srand(seed);
		for (iter = 0; iter < 100; iter++) {
			ikcpcb *kcp = ikcp_create(1, NULL);
			char data[16384];
			int step;
			if (kcp == NULL) return 1;
			ikcp_setoutput(kcp, output_cb);
			ikcp_nodelay(kcp, iter % 3, 10, iter % 4, iter % 2);
			kcp->stream = iter % 2;
			memset(data, 0x61, sizeof(data));
			ikcp_update(kcp, 1000);		// sets updated = 1
			for (step = 0; step < 30; step++) {
				if (rand() % 3) {
					ikcp_send(kcp, data, rand() % 16000);
				}
				if (rand() % 2) {
					ikcp_setmtu(kcp, 50 + rand() % 4000);
				}
				ikcp_update(kcp, 1010 + step * 10);
			}
			ikcp_release(kcp);
		}
	}
	return 0;
}

int main(void)
{
	if (test_shrink_with_pending() != 0) {
		printf("setmtu test FAILED (shrink with pending segments)\n");
		return 1;
	}
	if (test_random_interleave() != 0) {
		printf("setmtu test FAILED (random interleave)\n");
		return 1;
	}
	printf("setmtu regression test passed\n");
	return 0;
}
