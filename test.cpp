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
	printf("press enter to next ...\n");
	char ch; scanf("%c", &ch);
}

//=====================================================================
// 回归测试：issue #449
//
// ikcp_send 原来用常量 IKCP_WND_RCV 限制单次发送的分片数，即使通过
// ikcp_wndsize 调大了 rcv_wnd，也无法发送超过 IKCP_WND_RCV 个分片的
// 消息。现在上限跟随 rcv_wnd，同时消息模式仍受 8 位 frg 字段约束
// （最多 256 个分片），流模式不使用 frg，因此只受窗口约束。
//=====================================================================

// 一个无丢包、无延迟的本地链路
struct KcpPipe {
	std::vector<std::vector<char> > queue;
};

static int pipe_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	KcpPipe *pipe = (KcpPipe*)user;
	(void)kcp;
	pipe->queue.push_back(std::vector<char>(buf, buf + len));
	return 0;
}

static void pipe_pump(KcpPipe &pipe, ikcpcb *kcp)
{
	for (size_t i = 0; i < pipe.queue.size(); i++) {
		ikcp_input(kcp, &pipe.queue[i][0], (long)pipe.queue[i].size());
	}
	pipe.queue.clear();
}

// 在本地链路上发送 len 字节，返回 ikcp_send 的结果；收到的字节数
// 通过 *received 返回（-1 表示未完整收到）。
static int pipe_transfer(int rcvwnd, int len, int stream, int *received)
{
	KcpPipe p1, p2;
	ikcpcb *kcp1 = ikcp_create(0x44944944, &p1);
	ikcpcb *kcp2 = ikcp_create(0x44944944, &p2);
	char *data = (char*)malloc(len);
	char *recv = (char*)malloc(len + 16);
	int ret, total = 0, iter;
	IUINT32 now = 0;

	assert(kcp1 && kcp2 && data && recv);

	kcp1->output = pipe_output;
	kcp2->output = pipe_output;
	kcp1->stream = stream;
	kcp2->stream = stream;
	ikcp_wndsize(kcp1, 4096, rcvwnd);
	ikcp_wndsize(kcp2, 4096, rcvwnd);
	ikcp_nodelay(kcp1, 1, 10, 2, 1);
	ikcp_nodelay(kcp2, 1, 10, 2, 1);

	for (int i = 0; i < len; i++) data[i] = (char)((i * 31 + 7) & 0xff);

	ret = ikcp_send(kcp1, data, len);

	for (iter = 0; iter < 200000 && total < len; iter++) {
		int hr;
		now += 10;
		ikcp_update(kcp1, now);
		pipe_pump(p1, kcp2);
		ikcp_update(kcp2, now);
		pipe_pump(p2, kcp1);
		while ((hr = ikcp_recv(kcp2, recv + total, len + 16 - total)) > 0) {
			total += hr;
		}
	}

	if (ret == len && total == len && memcmp(data, recv, len) == 0) {
		*received = total;
	}	else {
		*received = -1;
	}

	ikcp_release(kcp1);
	ikcp_release(kcp2);
	free(data);
	free(recv);
	return ret;
}

static void test_issue_449(void)
{
	int mss, received, ret;

	printf("==== issue #449 regression tests ====\n");

	// 默认窗口下，单次最多 128 个分片（与旧行为一致）
	{
		ikcpcb *probe = ikcp_create(0x44944944, NULL);
		assert(probe != NULL);
		mss = (int)probe->mss;
		ikcp_release(probe);
	}

	ret = pipe_transfer((int)IKCP_WND_RCV, (int)IKCP_WND_RCV * mss, 0, &received);
	if (ret != -2) {
		printf("[FAIL] default window: 128 fragments should be rejected, got %d\n", ret);
		exit(1);
	}
	printf("[PASS] default rcv_wnd still rejects 128 fragments\n");

	// 调大 rcv_wnd 后，可以发送超过 IKCP_WND_RCV 个分片的消息
	ret = pipe_transfer(1024, 200 * mss, 0, &received);
	if (ret != 200 * mss || received != 200 * mss) {
		printf("[FAIL] enlarged window: 200 fragments, send=%d recv=%d\n", ret, received);
		exit(1);
	}
	printf("[PASS] enlarged rcv_wnd allows a 200-fragment message\n");

	// 消息模式的分片数上限是 8 位 frg 字段能表示的范围
	ret = pipe_transfer(4096, 256 * mss, 0, &received);
	if (ret != 256 * mss || received != 256 * mss) {
		printf("[FAIL] message mode: 256 fragments, send=%d recv=%d\n", ret, received);
		exit(1);
	}
	printf("[PASS] message mode allows 256 fragments (max frg)\n");

	ret = pipe_transfer(4096, 257 * mss, 0, &received);
	if (ret != -2) {
		printf("[FAIL] message mode: 257 fragments must be rejected, got %d\n", ret);
		exit(1);
	}
	printf("[PASS] message mode rejects 257 fragments (frg overflow)\n");

	// 流模式不使用 frg，因此只受窗口约束
	ret = pipe_transfer(4096, 300 * mss, 1, &received);
	if (ret != 300 * mss || received != 300 * mss) {
		printf("[FAIL] stream mode: 300 fragments, send=%d recv=%d\n", ret, received);
		exit(1);
	}
	printf("[PASS] stream mode allows 300 fragments\n");

	// 消息大于窗口时仍然被拒绝
	ret = pipe_transfer(128, 200 * mss, 0, &received);
	if (ret != -2) {
		printf("[FAIL] message larger than window must be rejected, got %d\n", ret);
		exit(1);
	}
	printf("[PASS] message larger than rcv_wnd is rejected\n");

	printf("==== issue #449 regression tests passed ====\n");
}

int main()
{
	test_issue_449();	// issue #449 回归测试
	test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
	test(1);	// 普通模式，关闭流控等
	test(2);	// 快速模式，所有开关都打开，且关闭流控
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/


