//=====================================================================
//
// test_check.cpp - deterministic tests for ikcp_check / ikcp_update
//
// Issue #448: ikcp_check could return a wakeup time earlier than
// ts_flush (because a segment is due for retransmission), but
// ikcp_update only called ikcp_flush when current >= ts_flush, so
// calling ikcp_update at the time ikcp_check returned did nothing.
// ikcp_update now also flushes early when a retransmission is due.
//
// Deterministic: no real time, no network, no randomness.
//
//=====================================================================

#include <stdio.h>
#include <string.h>

#include "ikcp.h"

static char out_buf[64][2048];
static int out_len[64];
static int out_count = 0;

static int test_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp; (void)user;
	if (out_count < 64 && len > 0 && len <= 2048) {
		memcpy(out_buf[out_count], buf, len);
		out_len[out_count] = len;
		out_count++;
	}
	return 0;
}

static void reset_out(void)
{
	out_count = 0;
}

static ikcpcb* make_kcp(void)
{
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	kcp->output = test_output;
	return kcp;
}

static int failures = 0;

static void check(int cond, const char *what)
{
	if (cond) {
		printf("PASS: %s\n", what);
	} else {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

// ikcp_check promises a wakeup earlier than ts_flush when a
// retransmission is due; ikcp_update at that time must actually
// send the retransmission.
static void test_check_wakeup_sends_retransmission(void)
{
	printf("-- test_check_wakeup_sends_retransmission\n");
	// large flush interval so ts_flush is far away from the rto deadline
	ikcpcb *kcp = make_kcp();
	ikcp_nodelay(kcp, 1, 5000, 2, 1);

	const char msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ikcp_send(kcp, msg, sizeof(msg));

	reset_out();
	ikcp_update(kcp, 1000);              // first update: flushes the new data
	check(out_count == 1, "first update sends data");

	// packet was sent at t=1000 with default rto of 200ms, so the
	// retransmission deadline is at 1200, way before ts_flush (6000)
	IUINT32 next = ikcp_check(kcp, 1000);
	check(next == 1200, "ikcp_check returns retransmission deadline (1200)");

	reset_out();
	ikcp_update(kcp, next);              // wakeup at the promised time
	check(out_count == 1, "ikcp_update at ikcp_check time retransmits");

	ikcp_release(kcp);
}

// no retransmission may be emitted before its deadline, even though
// the flush interval is 5000ms
static void test_no_early_retransmission(void)
{
	printf("-- test_no_early_retransmission\n");
	ikcpcb *kcp = make_kcp();
	ikcp_nodelay(kcp, 1, 5000, 2, 1);

	const char msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ikcp_send(kcp, msg, sizeof(msg));

	reset_out();
	ikcp_update(kcp, 1000);
	check(out_count == 1, "first update sends data");

	reset_out();
	ikcp_update(kcp, 1199);              // one tick before resendts
	check(out_count == 0, "no retransmission before resendts");

	ikcp_update(kcp, 1200);              // exactly at resendts
	check(out_count == 1, "retransmission at resendts");

	ikcp_release(kcp);
}

// a second ikcp_update at the same timestamp must not retransmit twice
// (the early flush must keep resendts forward-dated)
static void test_no_double_retransmission(void)
{
	printf("-- test_no_double_retransmission\n");
	ikcpcb *kcp = make_kcp();
	ikcp_nodelay(kcp, 1, 5000, 2, 1);

	const char msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ikcp_send(kcp, msg, sizeof(msg));

	reset_out();
	ikcp_update(kcp, 1000);
	ikcp_update(kcp, 1200);              // sends retransmission
	reset_out();
	ikcp_update(kcp, 1200);              // same timestamp again
	check(out_count == 0, "no duplicate retransmission at same time");

	ikcp_release(kcp);
}

// ikcp_check reports the current time when a retransmission is already
// due, and ikcp_update then retransmits immediately
static void test_check_reports_current_when_due(void)
{
	printf("-- test_check_reports_current_when_due\n");
	ikcpcb *kcp = make_kcp();
	ikcp_nodelay(kcp, 1, 5000, 2, 1);

	const char msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ikcp_send(kcp, msg, sizeof(msg));

	reset_out();
	ikcp_update(kcp, 1000);

	IUINT32 chk = ikcp_check(kcp, 1300);  // resendts (1200) already passed
	check(chk == 1300, "ikcp_check returns current time when retransmission is due");

	reset_out();
	ikcp_update(kcp, chk);
	check(out_count == 1, "ikcp_update retransmits immediately when due");

	ikcp_release(kcp);
}

// before the first ikcp_update call, ikcp_check returns the current time
static void test_check_before_first_update(void)
{
	printf("-- test_check_before_first_update\n");
	ikcpcb *kcp = make_kcp();
	IUINT32 chk = ikcp_check(kcp, 4242);
	check(chk == 4242, "ikcp_check returns current time before first ikcp_update");
	ikcp_release(kcp);
}

int main(void)
{
	test_check_wakeup_sends_retransmission();
	test_no_early_retransmission();
	test_no_double_retransmission();
	test_check_reports_current_when_due();
	test_check_before_first_update();

	if (failures) {
		printf("%d test(s) FAILED\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
