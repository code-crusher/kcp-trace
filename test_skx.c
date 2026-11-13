//=====================================================================
//
// test_skx.c - rcv_buf skip list tests (issue #331)
//
// Feeds out-of-order PUSH segments to a kcp receiver and checks the
// rcv_buf skip list index against a reference model, plus a
// large-window smoke test that catches the old O(N^2) insertion.
//
// build: gcc test_skx.c -o test_skx
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ikcp.h"
#include "ikcp.c"

// direct output: drop packets, this test drives the receive side only
static int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)len; (void)kcp; (void)user;
	return 0;
}

// feed one IKCP_CMD_PUSH segment carrying its sn as payload
static void push_segment(ikcpcb *kcp, IUINT32 sn)
{
	char buf[IKCP_OVERHEAD + 4];
	char *ptr = buf;
	ptr = ikcp_encode32u(ptr, kcp->conv);
	ptr = ikcp_encode8u(ptr, IKCP_CMD_PUSH);
	ptr = ikcp_encode8u(ptr, 0);
	ptr = ikcp_encode16u(ptr, (IUINT16)kcp->rcv_wnd);
	ptr = ikcp_encode32u(ptr, 1000);
	ptr = ikcp_encode32u(ptr, sn);
	ptr = ikcp_encode32u(ptr, 0);
	ptr = ikcp_encode32u(ptr, 4);
	ptr = ikcp_encode32u(ptr, sn);
	ikcp_input(kcp, buf, IKCP_OVERHEAD + 4);
}

static int tests_failed = 0;

static void check(int ok, const char *what, long a, long b)
{
	if (!ok) {
		printf("FAIL: %s (%ld vs %ld)\n", what, a, b);
		tests_failed = 1;
	}
}

//---------------------------------------------------------------------
// randomized out-of-order delivery against a reference model
//---------------------------------------------------------------------
static void test_out_of_order(void)
{
	int round;
	srand(12345);

	for (round = 0; round < 300; round++) {
		int wnd = 32 + rand() % 512;
		int eff_wnd = wnd > (int)IKCP_WND_RCV ? wnd : (int)IKCP_WND_RCV;
		int total = wnd + rand() % (4 * wnd);
		ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
		char *accepted;
		IUINT32 rcv_nxt_model = 0;   // kcp->rcv_nxt
		IUINT32 n_accepted = 0, n_delivered = 0, pushed = 0;
		char buf[2048];

		if (kcp == NULL) { printf("FAIL: create\n"); tests_failed = 1; return; }
		ikcp_setoutput(kcp, udp_output);
		ikcp_wndsize(kcp, 128, wnd);
		accepted = (char*)calloc(total + 16, 1);
		if (accepted == NULL) { printf("FAIL: oom\n"); tests_failed = 1; return; }

		// contiguous accepted segments move to rcv_queue while it
		// has room (nrcv_que < rcv_wnd)
		#define MODEL_SEQ() do { \
			while (rcv_nxt_model < (IUINT32)total && accepted[rcv_nxt_model] \
				&& (rcv_nxt_model - n_delivered) < (IUINT32)eff_wnd) \
				rcv_nxt_model++; \
		} while (0)

		// deliver sn = 0..total-1 in random order, with random
		// duplicates of already-sent sns interleaved
		while (n_delivered < (IUINT32)total) {
			int r = rand() % 10;
			if (r < 7 && pushed < (IUINT32)total) {
				IUINT32 sn = pushed++;
				long off = (long)(sn - rcv_nxt_model);
				if (off >= 0 && off < wnd && !accepted[sn]) {
					accepted[sn] = 1; n_accepted++;
				}
				push_segment(kcp, sn);
				if (rand() % 4 == 0 && pushed > 1) {
					push_segment(kcp, rand() % pushed);
				}
			}	else if (r < 8) {
				if (pushed > 0) push_segment(kcp, rand() % pushed);
			}	else {
				// drain everything the model says is queued;
				// data must come out strictly in sn order
				MODEL_SEQ();
				while (n_delivered < rcv_nxt_model) {
					if (ikcp_recv(kcp, buf, sizeof(buf)) < 0) {
						check(0, "round recv starved",
							(long)n_delivered, (long)rcv_nxt_model);
						goto out;
					}
					check(*(IUINT32*)buf == n_delivered,
						"round recv order",
						(long)*(IUINT32*)buf, (long)n_delivered);
					if (tests_failed) goto out;
					n_delivered++;
					MODEL_SEQ();
				}
			}
			MODEL_SEQ();
			check(kcp->rcv_nxt == rcv_nxt_model, "round rcv_nxt",
				(long)kcp->rcv_nxt, (long)rcv_nxt_model);
			check(kcp->nrcv_que == rcv_nxt_model - n_delivered,
				"round nrcv_que", (long)kcp->nrcv_que,
				(long)(rcv_nxt_model - n_delivered));
			check(kcp->nrcv_buf == n_accepted - rcv_nxt_model,
				"round nrcv_buf", (long)kcp->nrcv_buf,
				(long)(n_accepted - rcv_nxt_model));
			if (tests_failed) goto out;
		}

		// final drain: everything must come out in order
		while (n_delivered < (IUINT32)total) {
			if (ikcp_recv(kcp, buf, sizeof(buf)) < 0) {
				check(0, "final recv stuck", (long)n_delivered, total);
				goto out;
			}
			check(*(IUINT32*)buf == n_delivered, "final recv order",
				(long)*(IUINT32*)buf, (long)n_delivered);
			if (tests_failed) goto out;
			n_delivered++;
		}
		check(kcp->nrcv_buf == 0 && kcp->nrcv_que == 0,
			"leftover segments", (long)kcp->nrcv_buf, (long)kcp->nrcv_que);

	out:
		free(accepted);
		ikcp_release(kcp);
		if (tests_failed) return;
	}
	printf("test_out_of_order: 300 rounds OK\n");
}

//---------------------------------------------------------------------
// worst case for the old linear scan: strictly reverse order arrival
// over a large window (used to be O(N^2); must stay fast now)
//---------------------------------------------------------------------
static void test_reverse_large(void)
{
	const IUINT32 total = 20000;
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	char buf[2048];
	IUINT32 sn, expect = 0;
	clock_t t0, t1;

	if (kcp == NULL) { printf("FAIL: create\n"); tests_failed = 1; return; }
	ikcp_setoutput(kcp, udp_output);
	ikcp_wndsize(kcp, 128, (int)total);

	t0 = clock();
	for (sn = total - 1; ; sn--) {
		push_segment(kcp, sn);
		if (sn == 0) break;
	}
	t1 = clock();

	// with the old linear insertion this takes seconds; the skip
	// list finishes in milliseconds, so one second is a generous
	// upper bound that still catches the quadratic regression
	check(t1 - t0 <= CLOCKS_PER_SEC, "reverse order too slow",
		(long)(t1 - t0), (long)CLOCKS_PER_SEC);

	while (ikcp_recv(kcp, buf, sizeof(buf)) > 0) {
		check(*(IUINT32*)buf == expect, "reverse recv order",
			(long)*(IUINT32*)buf, (long)expect);
		if (tests_failed) break;
		expect++;
	}
	check(expect == total, "reverse delivered", (long)expect, (long)total);
	check(kcp->nrcv_buf == 0 && kcp->nrcv_que == 0, "reverse leftover",
		(long)kcp->nrcv_buf, (long)kcp->nrcv_que);

	printf("test_reverse_large: %lu reverse inserts in %.3fs OK\n",
		(unsigned long)total, (double)(t1 - t0) / CLOCKS_PER_SEC);
	ikcp_release(kcp);
}

int main(void)
{
	test_out_of_order();
	if (tests_failed) return 1;
	test_reverse_large();
	if (tests_failed) return 1;
	printf("all skx tests passed\n");
	return 0;
}
