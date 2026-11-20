//=====================================================================
//
// test410.cpp - deterministic tests for the input timestamp handling
// (issue #410)
//
// Issue #410: ikcp_input() samples RTT from kcp->current, which is only
// refreshed by ikcp_update(). An ACK that arrives between two update
// calls is therefore measured against a stale clock, so the RTT sample
// (and the RTO derived from it) is too small. The maintainer rejected
// changing the ikcp_input signature, and suggested either setting
// kcp->current by hand or refreshing the clock before feeding a batch
// of packets.
//
// The fix adds ikcp_input_with_current(), which takes the clock
// explicitly, so the batch workflow is available without touching the
// existing ABI. ikcp_input() keeps its old behavior.
//
// Deterministic: no real time, no network, no randomness.
//
//=====================================================================

#include <stdio.h>
#include <string.h>

#include "ikcp.c"

static int failures = 0;

static void check(int cond, const char *what)
{
	if (cond) {
		printf("PASS: %s\n", what);
	}	else {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

//---------------------------------------------------------------------
// output capture
//---------------------------------------------------------------------

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

//---------------------------------------------------------------------
// crafted input packets
//---------------------------------------------------------------------

static char *enc32(char *p, IUINT32 l)
{
	p[0] = (char)(l & 0xff);
	p[1] = (char)((l >> 8) & 0xff);
	p[2] = (char)((l >> 16) & 0xff);
	p[3] = (char)((l >> 24) & 0xff);
	return p + 4;
}

static char *enc16(char *p, IUINT16 w)
{
	p[0] = (char)(w & 0xff);
	p[1] = (char)((w >> 8) & 0xff);
	return p + 2;
}

// one header with the given command; 'len' bytes of payload follow when
// 'payload' is not NULL
static int make_seg(char *buf, IUINT32 conv, IUINT8 cmd, IUINT32 ts,
		IUINT32 sn, IUINT32 una, const char *payload, IUINT32 len)
{
	char *p = buf;
	p = enc32(p, conv);
	*p++ = (char)cmd;
	*p++ = 0;								// frg
	p = enc16(p, 128);						// wnd
	p = enc32(p, ts);
	p = enc32(p, sn);
	p = enc32(p, una);
	p = enc32(p, len);
	if (payload && len > 0) {
		memcpy(p, payload, len);
		p += len;
	}
	return (int)(p - buf);
}

static int make_ack(char *buf, IUINT32 conv, IUINT32 ts, IUINT32 sn,
		IUINT32 una)
{
	return make_seg(buf, conv, IKCP_CMD_ACK, ts, sn, una, NULL, 0);
}

//---------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------

// a sender with nsegs small messages flushed at 'ts_flush_at', so every
// segment in snd_buf carries ts = ts_flush_at
static ikcpcb* make_sender(int nsegs, IUINT32 ts_flush_at)
{
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	char buffer[8];
	int i;

	kcp->output = test_output;
	// fast mode: nodelay=1 (rx_minrto=30), interval=10, fastresend=2
	ikcp_nodelay(kcp, 1, 10, 2, 1);

	for (i = 0; i < nsegs; i++) {
		ikcp_send(kcp, buffer, 8);
	}
	ikcp_update(kcp, ts_flush_at);
	reset_out();
	return kcp;
}

static IKCPSEG* find_seg(ikcpcb *kcp, IUINT32 sn)
{
	struct IQUEUEHEAD *p;
	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		if (seg->sn == sn) return seg;
	}
	return NULL;
}

//---------------------------------------------------------------------
// issue #410: an ack for a segment sent at t=1000 reaches the sender at
// t=1050, but ikcp_update was not called in between. ikcp_input keeps
// using the stale kcp->current (0ms sample), while
// ikcp_input_with_current samples the real 50ms.
//---------------------------------------------------------------------

static void test_explicit_current_fixes_rtt(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_explicit_current_fixes_rtt\n");
	kcp = make_sender(1, 1000);

	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	ikcp_input_with_current(kcp, 1050, pkt, len);

	check(kcp->rx_srtt == 50,
		"rtt sample is 50ms, taken from the explicit current");
	check(kcp->rx_rto == 150,
		"rto follows the real rtt (50 + 4*25), not the stale clock");

	ikcp_release(kcp);
}

static void test_plain_input_keeps_old_behavior(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_plain_input_keeps_old_behavior\n");
	kcp = make_sender(1, 1000);

	// kcp->current is still 1000 here: the old entry point measures
	// against it, exactly as before the change
	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	ikcp_input(kcp, pkt, len);

	check(kcp->rx_srtt == 0,
		"ikcp_input still samples from kcp->current (stale here)");
	check(kcp->rx_rto == 30,
		"ikcp_input keeps the old rto outcome (bounded by rx_minrto)");

	ikcp_release(kcp);
}

static void test_manual_current_workaround(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_manual_current_workaround\n");
	kcp = make_sender(1, 1000);

	// the maintainer's suggested workaround still works
	kcp->current = 1040;
	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	ikcp_input(kcp, pkt, len);

	check(kcp->rx_srtt == 40,
		"setting kcp->current by hand still feeds the rtt estimator");

	ikcp_release(kcp);
}

static void test_explicit_current_is_stored(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_explicit_current_is_stored\n");
	kcp = make_sender(1, 1000);

	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	ikcp_input_with_current(kcp, 2000, pkt, len);

	check(kcp->current == 2000,
		"kcp->current is refreshed to the explicit timestamp");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// a batch of packets received at the same time shares one timestamp,
// the workflow recommended in the issue thread
//---------------------------------------------------------------------

static void test_batch_uses_one_timestamp(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_batch_uses_one_timestamp\n");
	kcp = make_sender(2, 1000);

	len = make_ack(pkt, kcp->conv, 1000, 0, 2);
	len += make_ack(pkt + len, kcp->conv, 1000, 1, 2);
	ikcp_input_with_current(kcp, 1050, pkt, len);

	check(find_seg(kcp, 0) == NULL && find_seg(kcp, 1) == NULL,
		"both segments are acked by the batch");
	check(kcp->rx_srtt == 50,
		"both acks are sampled with the batch timestamp");

	ikcp_release(kcp);
}

static void test_no_sample_when_clock_precedes_ts(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_no_sample_when_clock_precedes_ts\n");
	kcp = make_sender(1, 1000);

	// a current older than the segment ts must not produce a negative
	// sample (the guard in ikcp_input is preserved)
	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	ikcp_input_with_current(kcp, 900, pkt, len);

	check(kcp->rx_srtt == 0 && kcp->rx_rto == 200,
		"no rtt sample when the timestamp predates the segment");
	check(find_seg(kcp, 0) == NULL, "the ack is still processed");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// the congestion control callback sees the explicit timestamp too
//---------------------------------------------------------------------

static IINT32 captured_rtt = -12345;
static IUINT32 captured_sn = 0;

static void test_on_pkt_acked(ikcpcb *kcp, IUINT32 sn, IUINT32 ts,
		IUINT32 len, IINT32 rtt, IUINT32 xmit)
{
	(void)kcp; (void)ts; (void)len; (void)xmit;
	captured_rtt = rtt;
	captured_sn = sn;
}

// fields: name, init, release, on_ack, on_fast_retransmit, on_timeout,
// on_tick, on_app_limited, on_rtt, on_pkt_sent, on_pkt_acked, get_info,
// pacing_rate
static const struct IKCPOPS test_ops = {
	"test", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	test_on_pkt_acked, NULL, NULL
};

static void test_ccops_sees_explicit_current(void)
{
	ikcpcb *kcp;
	char pkt[64];
	int len;

	printf("-- test_ccops_sees_explicit_current\n");
	kcp = make_sender(1, 1000);
	ikcp_setcc(kcp, &test_ops);
	reset_out();

	// una=0 keeps the segment in snd_buf so the callback still sees it
	len = make_ack(pkt, kcp->conv, 1000, 0, 0);
	ikcp_input_with_current(kcp, 1050, pkt, len);

	check(captured_sn == 0, "on_pkt_acked is called for the acked segment");
	check(captured_rtt == 50,
		"on_pkt_acked receives the rtt from the explicit current");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// packet parsing and error codes are unchanged
//---------------------------------------------------------------------

static void test_error_codes_unchanged(void)
{
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	char pkt[64];
	char bad[64];
	int len;

	printf("-- test_error_codes_unchanged\n");
	kcp->output = test_output;

	check(ikcp_input_with_current(kcp, 1000, NULL, 24) == -1,
		"NULL data returns -1");
	check(ikcp_input_with_current(kcp, 1000, pkt, 10) == -1,
		"a packet shorter than the header returns -1");

	len = make_ack(pkt, 0x55667788, 1000, 0, 1);
	check(ikcp_input_with_current(kcp, 1000, pkt, len) == -1,
		"a mismatched conv returns -1");

	len = make_ack(pkt, kcp->conv, 1000, 0, 1);
	enc32(pkt + 20, 100);					// len field > payload
	check(ikcp_input_with_current(kcp, 1000, pkt, len) == -2,
		"an oversized length field returns -2");

	memcpy(bad, pkt, sizeof(pkt));
	len = make_ack(bad, kcp->conv, 1000, 0, 1);
	bad[4] = (char)0x7f;					// invalid command
	check(ikcp_input_with_current(kcp, 1000, bad, len) == -3,
		"an invalid command returns -3");

	ikcp_release(kcp);
}

static void test_push_does_not_sample_rtt(void)
{
	ikcpcb *kcp;
	char pkt[64];
	char payload[4] = { 1, 2, 3, 4 };
	int len;

	printf("-- test_push_does_not_sample_rtt\n");
	kcp = ikcp_create(0x11223344, (void*)0);
	kcp->output = test_output;
	ikcp_nodelay(kcp, 1, 10, 2, 1);

	len = make_seg(pkt, kcp->conv, IKCP_CMD_PUSH, 1000, 0, 0,
			payload, sizeof(payload));
	check(ikcp_input_with_current(kcp, 1050, pkt, len) == 0,
		"a data packet is accepted");
	check(kcp->rx_srtt == 0 && kcp->rx_rto == 200,
		"data packets do not feed the rtt estimator");
	check(kcp->current == 1050,
		"the clock is refreshed even for data packets");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// end to end: a sender whose acks are fed with explicit timestamps
// measures the real rtt; the receiver's clock is refreshed as well
//---------------------------------------------------------------------

static void test_end_to_end_rtt(void)
{
	ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);
	char buffer[64];

	printf("-- test_end_to_end_rtt\n");
	kcp1->output = test_output;
	kcp2->output = test_output;
	ikcp_nodelay(kcp1, 1, 10, 2, 1);
	ikcp_nodelay(kcp2, 1, 10, 2, 1);

	// send one message at t=1000
	ikcp_send(kcp1, "hello", 5);
	ikcp_update(kcp1, 1000);
	check(out_count == 1, "one data packet is emitted");

	// the packet reaches the peer at t=1050
	reset_out();
	check(ikcp_input_with_current(kcp2, 1050, out_buf[0], out_len[0]) == 0,
		"the peer accepts the data packet");
	check(kcp2->current == 1050, "the peer clock is refreshed");
	check(ikcp_recv(kcp2, buffer, sizeof(buffer)) == 5,
		"the payload is delivered");

	// the peer acks at t=1050; the ack reaches the sender at t=1100
	reset_out();
	ikcp_update(kcp2, 1050);
	check(out_count == 1, "the peer emits an ack");
	check(ikcp_input_with_current(kcp1, 1100, out_buf[0], out_len[0]) == 0,
		"the sender accepts the ack");

	check(kcp1->rx_srtt == 100,
		"the sender measures the full 100ms round trip");

	ikcp_release(kcp1);
	ikcp_release(kcp2);
}

int main(void)
{
	test_explicit_current_fixes_rtt();
	test_plain_input_keeps_old_behavior();
	test_manual_current_workaround();
	test_explicit_current_is_stored();
	test_batch_uses_one_timestamp();
	test_no_sample_when_clock_precedes_ts();
	test_ccops_sees_explicit_current();
	test_error_codes_unchanged();
	test_push_does_not_sample_rtt();
	test_end_to_end_rtt();

	if (failures > 0) {
		printf("== %d test(s) failed ==\n", failures);
		return 1;
	}
	printf("== all input timestamp tests passed ==\n");
	return 0;
}
