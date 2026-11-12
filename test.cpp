//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "ikcp.c"


// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(10, 60, 125);

	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	kcp1->output = udp_output;
	kcp2->output = udp_output;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);

	// 判断测试用例的模式
	if (mode == 0) {
		// 默认模式
		ikcp_nodelay(kcp1, 0, 10, 0, 0);
		ikcp_nodelay(kcp2, 0, 10, 0, 0);
	}
	else if (mode == 1) {
		// 普通模式，关闭流控等
		ikcp_nodelay(kcp1, 0, 10, 0, 1);
		ikcp_nodelay(kcp2, 0, 10, 0, 1);
	}	else {
		// 启动快速模式
		// 第二个参数 nodelay-启用以后若干常规加速将启动
		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
		// 第四个参数 resend为快速重传指标，设置为2
		// 第五个参数 为是否禁用常规流控，这里禁止
		ikcp_nodelay(kcp1, 2, 10, 2, 1);
		ikcp_nodelay(kcp2, 2, 10, 2, 1);
		kcp1->rx_minrto = 10;
		kcp1->fastresend = 1;
	}


	char buffer[2000];
	int hr;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		ikcp_update(kcp1, iclock());
		ikcp_update(kcp2, iclock());

		// 每隔 20ms，kcp1发送数据
		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;

			// 发送上层协议包
			ikcp_send(kcp1, buffer, 8);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			ikcp_input(kcp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
			ikcp_input(kcp1, buffer, hr);
		}

		// kcp2接收到任何包都返回回去
		while (1) {
			hr = ikcp_recv(kcp2, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			ikcp_send(kcp2, buffer, hr);
		}

		// kcp1收到kcp2的回射数据
		while (1) {
			hr = ikcp_recv(kcp1, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;
			
			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			printf("[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
		}
		if (next > 1000) break;
	}

	ts1 = iclock() - ts1;

	ikcp_release(kcp1);
	ikcp_release(kcp2);

	const char *names[3] = { "default", "normal", "fast" };
	printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
	delete vnet;
	vnet = NULL;
	// 管道/CI 下 stdin 不是终端，不要阻塞
	if (isatty(0)) {
		printf("press enter to next ...\n");
		char ch; scanf("%c", &ch);
	}
}


//=====================================================================
// 回归测试：issue #348
//   服务端底层 UDP 已经收到数据，ikcp_recv 却返回 -1。
//
// 结论：ikcp_recv 返回 -1 表示“接收队列为空，暂时没有完整的消息可
// 交付”，并不是 ikcp_input 出错。数据包只有满足下面条件才会被交付：
//   1. 是 PUSH 数据包（ACK/窗口探测等控制包本来就没有上层数据）；
//   2. 不是重复包（对端重传的旧包会被丢弃）；
//   3. 顺序到达（缺失的 sn 没到之前，后面的包先缓存在 rcv_buf）。
//
// 另一个容易忽略的点：ikcp_input 只是把 ACK 放进待发送队列，真正把
// ACK 发出去的是 ikcp_update -> ikcp_flush。服务端若只在循环外调用
// 一次 ikcp_update（issue 中的写法），对端就收不到 ACK、不停重传，
// 服务端收到的全是重复包，ikcp_recv 自然一直返回 -1。
//=====================================================================

static int issue348_failures = 0;

static void issue348_expect(int cond, const char *what)
{
	if (!cond) {
		printf("[FAIL] %s\n", what);
		issue348_failures++;
	}
}

// 构造一个 KCP 数据包：conv + 24 字节头部 + len 字节载荷
static int issue348_build_seg(char *out, IUINT32 conv, IUINT32 cmd, IUINT32 frg,
	IUINT32 sn, IUINT32 una, const char *data, IUINT32 len)
{
	char *p = out;
	p = ikcp_encode32u(p, conv);
	p = ikcp_encode8u(p, (IUINT8)cmd);
	p = ikcp_encode8u(p, (IUINT8)frg);
	p = ikcp_encode16u(p, 128);
	p = ikcp_encode32u(p, 0);		// ts
	p = ikcp_encode32u(p, sn);
	p = ikcp_encode32u(p, una);
	p = ikcp_encode32u(p, len);
	if (len > 0 && data) {
		memcpy(p, data, len);
		p += len;
	}
	return (int)(p - out);
}

// 双向“网线”：分别保存客户端->服务端、服务端->客户端最近一个数据包
static char issue348_c2s[2048];
static char issue348_s2c[2048];
static int issue348_c2s_len = 0;
static int issue348_s2c_len = 0;

static int issue348_client_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp; (void)user;
	if (len > (int)sizeof(issue348_c2s)) len = (int)sizeof(issue348_c2s);
	memcpy(issue348_c2s, buf, len);
	issue348_c2s_len = len;
	return 0;
}

static int issue348_server_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp; (void)user;
	if (len > (int)sizeof(issue348_s2c)) len = (int)sizeof(issue348_s2c);
	memcpy(issue348_s2c, buf, len);
	issue348_s2c_len = len;
	return 0;
}

void test_issue_348(void)
{
	char packet[2048];
	char buffer[2000];
	IUINT32 conv = 0x11223344;
	int hr;

	printf("==== issue #348 regression tests ====\n");

	// ---- 1. 完整、按序的 PUSH 包：input 之后 recv 必须拿到数据 ----
	// 接收侧不依赖 ikcp_update，input 之后可以直接 recv
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv, IKCP_CMD_PUSH, 0, 0, 0, "bye", 3);
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "in-order push accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == 3,
			"in-order push delivered by ikcp_recv");
		issue348_expect(memcmp(buffer, "bye", 3) == 0, "payload intact");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1,
			"empty queue returns -1 (not an error)");
		ikcp_release(kcp);
	}

	// ---- 1b. 25 字节数据包（24 字节头部 + 1 字节数据），与 issue 截图一致 ----
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv, IKCP_CMD_PUSH, 0, 0, 0, "A", 1);
		issue348_expect(hr == 25, "segment is 24-byte header + 1-byte payload");
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "25-byte datagram accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == 1 &&
			buffer[0] == 'A', "25-byte datagram delivered");
		ikcp_release(kcp);
	}

	// ---- 2. 纯 ACK 数据包：input 返回 0，但 recv 返回 -1 是正常的 ----
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv, IKCP_CMD_ACK, 0, 0, 0, NULL, 0);
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "ACK-only datagram accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1,
			"ACK-only datagram carries no upper-level data");
		ikcp_release(kcp);
	}

	// ---- 3. 重复包：第二次 input 成功，但 recv 返回 -1 ----
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv, IKCP_CMD_PUSH, 0, 0, 0, "dup", 3);
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "first push accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == 3, "first push delivered");
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "duplicate push accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1,
			"duplicate push is dropped, not delivered twice");
		ikcp_release(kcp);
	}

	// ---- 4. 乱序包：先缓存，缺失的 sn 到达后按序交付 ----
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv, IKCP_CMD_PUSH, 0, 1, 0, "B", 1);
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "out-of-order push accepted");
		issue348_expect(kcp->nrcv_buf == 1, "out-of-order push buffered in rcv_buf");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1,
			"out-of-order push is not deliverable yet");

		hr = issue348_build_seg(packet, conv, IKCP_CMD_PUSH, 0, 0, 0, "A", 1);
		issue348_expect(ikcp_input(kcp, packet, hr) == 0, "missing push accepted");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == 1 &&
			buffer[0] == 'A', "buffered data delivered in order (A)");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == 1 &&
			buffer[0] == 'B', "buffered data delivered in order (B)");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1, "queue drained");
		ikcp_release(kcp);
	}

	// ---- 5. conv 不一致：input 直接返回 -1 ----
	{
		ikcpcb *kcp = ikcp_create(conv, (void*)0);
		assert(kcp != NULL);
		ikcp_wndsize(kcp, 128, 128);

		hr = issue348_build_seg(packet, conv ^ 0xffffffff, IKCP_CMD_PUSH,
			0, 0, 0, "X", 1);
		issue348_expect(ikcp_input(kcp, packet, hr) == -1, "mismatched conv rejected");
		issue348_expect(ikcp_recv(kcp, buffer, sizeof(buffer)) == -1, "nothing received");
		ikcp_release(kcp);
	}

	// ---- 6. 收到数据后必须继续 update，否则 ACK 发不出去 ----
	{
		IUINT32 now = 1000;
		ikcpcb *srv = ikcp_create(conv, (void*)0);
		ikcpcb *cli = ikcp_create(conv, (void*)1);
		assert(srv != NULL && cli != NULL);

		srv->output = issue348_server_output;
		cli->output = issue348_client_output;
		ikcp_wndsize(srv, 128, 128);
		ikcp_wndsize(cli, 128, 128);
		ikcp_nodelay(srv, 2, 10, 2, 1);
		ikcp_nodelay(cli, 2, 10, 2, 1);

		issue348_c2s_len = 0;
		issue348_s2c_len = 0;

		ikcp_update(srv, now);		// issue 中服务端唯一的一次 update
		ikcp_update(cli, now);
		ikcp_send(cli, "bye", 3);
		now += 10;
		ikcp_update(cli, now);		// 客户端把数据发出去

		issue348_expect(issue348_c2s_len == (int)(IKCP_OVERHEAD + 3),
			"client emitted a 27-byte KCP datagram");

		issue348_expect(ikcp_input(srv, issue348_c2s, issue348_c2s_len) == 0,
			"server accepted the datagram");
		issue348_expect(ikcp_recv(srv, buffer, sizeof(buffer)) == 3 &&
			memcmp(buffer, "bye", 3) == 0, "server delivered the payload");

		// 服务端没有 update：ACK 还留在待发送队列里，对端收不到确认
		issue348_expect(issue348_s2c_len == 0,
			"no ACK is sent until ikcp_update is called");

		// 对端超时重传，服务端再次 input 成功，但重复包不会再交付
		now += 300;
		issue348_c2s_len = 0;
		ikcp_update(cli, now);
		issue348_expect(issue348_c2s_len > 0, "client retransmits while unacked");
		issue348_expect(ikcp_input(srv, issue348_c2s, issue348_c2s_len) == 0,
			"server accepted the retransmission");
		issue348_expect(ikcp_recv(srv, buffer, sizeof(buffer)) == -1,
			"retransmitted duplicate yields -1");

		// 服务端补上 update 后 ACK 才会发出，对端 snd_una 前进、停止重传
		ikcp_update(srv, now);
		issue348_expect(issue348_s2c_len > 0, "server flushed ACK after update");
		issue348_expect(ikcp_input(cli, issue348_s2c, issue348_s2c_len) == 0,
			"client accepted the ACK");
		issue348_expect(cli->snd_una == 1, "client snd_una advanced by the ACK");

		ikcp_release(srv);
		ikcp_release(cli);
	}

	if (issue348_failures == 0) {
		printf("==== issue #348 regression tests passed ====\n");
	}	else {
		printf("==== issue #348 regression tests FAILED (%d) ====\n",
			issue348_failures);
	}
}

int main()
{
	test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
	test(1);	// 普通模式，关闭流控等
	test(2);	// 快速模式，所有开关都打开，且关闭流控
	test_issue_348();
	return issue348_failures ? 1 : 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/


