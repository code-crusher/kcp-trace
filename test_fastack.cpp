//=====================================================================
//
// test_fastack.cpp - deterministic tests for the maxack/latest_ts
// handling in ikcp_input (issue #422)
//
// Issue #422: with IKCP_FASTACK_CONSERVE, ikcp_input only accepted a
// new (maxack, latest_ts) pair when the acked sn was larger AND its ts
// was newer than latest_ts. An ack carrying a larger sn with an older
// or equal ts was dropped entirely, so maxack stalled and the segments
// between the old and the real maxack missed their fastack++ in that
// batch, delaying fast retransmit until the next ack wave or the RTO.
// This happens easily in practice: segments sent within the same flush
// burst share the same ts, and acks of original copies predate the ts
// of already retransmitted segments.
//
// The fix decouples the two updates: maxack moves on any larger acked
// sn, latest_ts moves only on a newer ts. The ts based conservation
// (not double counting the fastack of already retransmitted segments)
// stays in ikcp_parse_fastack.
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

static char out_buf[256][2048];
static int out_len[256];
static int out_count = 0;

static int test_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp; (void)user;
	if (out_count < 256 && len > 0 && len <= 2048) {
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

static IUINT32 dec32(const char *p)
{
	return ((IUINT32)(IUINT8)p[0]) | (((IUINT32)(IUINT8)p[1]) << 8) |
		(((IUINT32)(IUINT8)p[2]) << 16) | (((IUINT32)(IUINT8)p[3]) << 24);
}

// collect the sns of all data segments written to the output (one
// output call may batch multiple segments into a single datagram)
static int collect_out_sn(IUINT32 *sns, int maxsns)
{
	int i, count = 0;
	for (i = 0; i < out_count && count < maxsns; i++) {
		const char *p = out_buf[i];
		int left = out_len[i];
		while (left >= (int)IKCP_OVERHEAD && count < maxsns) {
			IUINT32 len = dec32(p + 20);
			if (p[4] == (char)IKCP_CMD_PUSH && count < maxsns) {
				sns[count++] = dec32(p + 12);
			}
			p += (int)IKCP_OVERHEAD + (int)len;
			left -= (int)IKCP_OVERHEAD + (int)len;
		}
	}
	return count;
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

// build one IKCP_CMD_ACK header (the ack's ts field is chosen by the
// test, ikcp_input never derives it)
static int make_ack(char *buf, IUINT32 conv, IUINT32 ts, IUINT32 sn, IUINT32 una)
{
	char *p = buf;
	IUINT32 wnd = 128, len = 0;
	p = enc32(p, conv);
	*p++ = (char)IKCP_CMD_ACK;
	*p++ = 0;								// frg
	*p++ = (char)(wnd & 0xff);
	*p++ = (char)((wnd >> 8) & 0xff);
	p = enc32(p, ts);
	p = enc32(p, sn);
	p = enc32(p, una);
	p = enc32(p, len);
	return (int)(p - buf);
}

//---------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------

// a sender with nsegs one-mss-or-less messages flushed at ts_flush_at,
// so all segments in snd_buf carry ts = ts_flush_at
static ikcpcb* make_sender(int nsegs, IUINT32 ts_flush_at)
{
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	char buffer[8];
	int i;

	kcp->output = test_output;
	// fast mode: fastresend=2, interval=10, rtomin=0
	ikcp_nodelay(kcp, 1, 10, 2, 1);

	for (i = 0; i < nsegs; i++) {
		ikcp_send(kcp, buffer, 8);
	}
	ikcp_update(kcp, ts_flush_at);	// all segments sent at ts_flush_at
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
// issue #422: acks of one burst share the same ts; the peer received
// sn=3 and sn=5 while sn=4 is lost. the second (larger) ack used to be
// dropped because its ts is not newer, stalling maxack at sn=3, so
// sn=4 missed its fastack++ and could not fast retransmit in time.
//---------------------------------------------------------------------

static void test_equal_ts_acks_advance_maxack(void)
{
	ikcpcb *kcp;
	char pkt[IKCP_OVERHEAD * 2];
	IUINT32 sns[16];
	int len, n;

	printf("-- test_equal_ts_acks_advance_maxack\n");
	kcp = make_sender(6, 1000);

	len = make_ack(pkt, kcp->conv, 1500, 3, 0);
	len += make_ack(pkt + len, kcp->conv, 1500, 5, 0);
	ikcp_input(kcp, pkt, len);

	check(find_seg(kcp, 4) != NULL, "sn=4 is still in snd_buf");
	check(find_seg(kcp, 4) && find_seg(kcp, 4)->fastack == 1,
		"sn=4 gets fastack++ from the equal-ts batch (issue #422)");

	// one increment stays below the fastresend threshold (2)
	ikcp_update(kcp, 1050);
	check(out_count == 0, "single increment does not trigger fast retransmit");

	// a second identical batch raises fastack to the threshold
	ikcp_input(kcp, pkt, len);
	check(find_seg(kcp, 4) && find_seg(kcp, 4)->fastack == 2,
		"sn=4 reaches the fastresend threshold after the second batch");

	// fast retransmit fires before the RTO deadline (resendts=1200)
	ikcp_update(kcp, 1150);
	n = collect_out_sn(sns, 16);
	check(n == 4 && sns[0] == 0 && sns[1] == 1 && sns[2] == 2 && sns[3] == 4,
		"sn=4 is fast retransmitted before its RTO, together with sn=0,1,2");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// the same stall with an older ts: sn=3 was retransmitted, so its ack
// echoes the newer retransmission ts, while the ack for sn=5 echoes
// the ts of sn=5's original copy (older). sn=5 must still advance
// maxack.
//---------------------------------------------------------------------

static void test_older_ts_ack_advances_maxack(void)
{
	ikcpcb *kcp;
	char pkt[IKCP_OVERHEAD * 2];
	int len;

	printf("-- test_older_ts_ack_advances_maxack\n");
	kcp = make_sender(6, 1000);

	len = make_ack(pkt, kcp->conv, 1600, 3, 0);
	len += make_ack(pkt + len, kcp->conv, 1100, 5, 0);
	ikcp_input(kcp, pkt, len);

	check(find_seg(kcp, 4) != NULL, "sn=4 is still in snd_buf");
	check(find_seg(kcp, 4) && find_seg(kcp, 4)->fastack == 1,
		"sn=4 gets fastack++ although sn=5's ack carries an older ts (issue #422)");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// conservation must survive the decoupling: after an RTO flush at
// t=1200 every snd_buf copy carries ts=1200; acks echoing the ts of
// the original copies (ts=1000) must not count them, no matter how
// far maxack advances.
//---------------------------------------------------------------------

static void test_retransmitted_segments_stay_conserved(void)
{
	ikcpcb *kcp;
	char pkt[IKCP_OVERHEAD * 2];
	int len, i, ok;

	printf("-- test_retransmitted_segments_stay_conserved\n");
	kcp = make_sender(6, 1000);

	// RTO retransmits everything at t=1200: snd_buf copies now carry ts=1200
	ikcp_update(kcp, 1200);
	reset_out();

	len = make_ack(pkt, kcp->conv, 1000, 3, 0);
	len += make_ack(pkt + len, kcp->conv, 1000, 5, 0);

	for (i = 0; i < 2; i++) {
		ikcp_input(kcp, pkt, len);
	}

	ok = 1;
	for (i = 0; i < 6; i++) {
		IKCPSEG *seg = find_seg(kcp, (IUINT32)i);
		if (seg && seg->fastack != 0) ok = 0;
	}
	check(ok, "acks of original copies do not count retransmitted segments");

	// hence no fast retransmit may fire (the next RTO is due at 1500)
	ikcp_update(kcp, 1210);
	check(out_count == 0, "conserved segments are not re-retransmitted early");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// sanity: the plain fastack path is untouched
//---------------------------------------------------------------------

static void test_single_ack_still_counts_lower_segments(void)
{
	ikcpcb *kcp;
	char pkt[IKCP_OVERHEAD];
	int len, ok;

	printf("-- test_single_ack_still_counts_lower_segments\n");
	kcp = make_sender(6, 1000);

	len = make_ack(pkt, kcp->conv, 1500, 2, 0);
	ikcp_input(kcp, pkt, len);

	ok = find_seg(kcp, 0) && find_seg(kcp, 0)->fastack == 1 &&
		find_seg(kcp, 1) && find_seg(kcp, 1)->fastack == 1;
	check(ok, "a plain ack still counts the lower unacked segments");

	ikcp_release(kcp);
}

//---------------------------------------------------------------------
// a later ack with a lower sn must not rewind maxack, but its ts may
// still raise latest_ts: sn=4 is (re)sent at t=1550, sn=5's ack echoes
// t=1500 (older than 1550), then sn=3's ack echoes t=1600 (newer).
// sn=3's copy postdates sn=4's current copy, so it is a fresh loss
// signal and sn=4 must be counted; sn=2 is marked as (re)sent at
// t=1650, newer than every acked ts, and must stay conserved.
//---------------------------------------------------------------------

static void test_lower_sn_ack_does_not_rewind_maxack(void)
{
	ikcpcb *kcp;
	char pkt[IKCP_OVERHEAD * 2];
	int len;
	IKCPSEG *seg;

	printf("-- test_lower_sn_ack_does_not_rewind_maxack\n");
	kcp = make_sender(6, 1000);

	seg = find_seg(kcp, 4);
	seg->ts = 1550;
	find_seg(kcp, 2)->ts = 1650;

	len = make_ack(pkt, kcp->conv, 1500, 5, 0);
	len += make_ack(pkt + len, kcp->conv, 1600, 3, 0);
	ikcp_input(kcp, pkt, len);

	check(seg->fastack == 1,
		"sn=4 is counted: the newer-ts lower-sn ack postdates its copy");
	check(find_seg(kcp, 2) && find_seg(kcp, 2)->fastack == 0,
		"sn=2 stays conserved: no acked ts postdates its copy");
	check(find_seg(kcp, 0) && find_seg(kcp, 0)->fastack == 1,
		"maxack did not rewind to sn=3, lower segments still counted");

	ikcp_release(kcp);
}

int main(void)
{
	test_equal_ts_acks_advance_maxack();
	test_older_ts_ack_advances_maxack();
	test_retransmitted_segments_stay_conserved();
	test_single_ack_still_counts_lower_segments();
	test_lower_sn_ack_does_not_rewind_maxack();

	if (failures > 0) {
		printf("== %d test(s) failed ==\n", failures);
		return 1;
	}
	printf("== all fastack tests passed ==\n");
	return 0;
}

