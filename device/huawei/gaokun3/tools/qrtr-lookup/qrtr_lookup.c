/*
 * gaokun3-qrtr-lookup —— 列出 QRTR 总线上注册的服务
 *
 * 为什么要自己写：Linux 侧诊断传感器用的是 qrtr-tools 的 qrtr-lookup，
 * 而 AOSP 里【没有】任何 QRTR 用户态工具。判断"SLPI 上的 SSC 起来了没有"
 * 的判据就是服务 400（Snapdragon Sensor Core）在不在，没工具就没法判。
 *
 * 这段代码同时是将来 sensors HAL 的地基：HAL 要在 AF_QIPCRTR 上跟服务 400
 * 说 QMI，握手第一步就是这里的 lookup。
 * （libssc 走的是 glib + libqmi 那一套，搬不进 Android，得照协议重写。）
 *
 * 全部常量都从 bionic 的 uapi 头里取，不凭记忆：
 *   AF_QIPCRTR 42            bionic/libc/include/sys/socket.h:169
 *   QRTR_PORT_CTRL           bionic/libc/kernel/uapi/linux/qrtr.h
 *   QRTR_TYPE_NEW_LOOKUP 10  同上
 *   QRTR_TYPE_NEW_SERVER 4   同上
 *   struct qrtr_ctrl_pkt     同上（__packed，server 分支是 4 个 __le32）
 *
 * 用法：
 *   gaokun3-qrtr-lookup          列出全部服务
 *   gaokun3-qrtr-lookup 400      只看传感器服务
 * 退出码：0 = 至少找到一个；2 = 一个都没有（便于脚本判定）。
 *
 * 背景与整条通路见 docs/dev/sensors-ssc-protocol.md。
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/qrtr.h>

int main(int argc, char **argv)
{
	unsigned int want = (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 0) : 0;
	struct sockaddr_qrtr sq;
	struct qrtr_ctrl_pkt pkt;
	socklen_t slen = sizeof(sq);
	int fd, found = 0;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "socket(AF_QIPCRTR): %s\n"
				"（内核缺 CONFIG_QRTR 时会是 EAFNOSUPPORT）\n",
			strerror(errno));
		return 1;
	}

	/* 控制包必须发给【本节点】的控制端口，本节点号问内核要 */
	if (getsockname(fd, (struct sockaddr *)&sq, &slen) < 0) {
		fprintf(stderr, "getsockname: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("本地 QRTR 节点 = %u\n", sq.sq_node);

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = want;      /* 0 = 不过滤 */
	pkt.server.instance = 0;

	sq.sq_port = QRTR_PORT_CTRL;
	if (sendto(fd, &pkt, sizeof(pkt), 0,
		   (struct sockaddr *)&sq, sizeof(sq)) < 0) {
		fprintf(stderr, "sendto(NEW_LOOKUP): %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("%-8s %-9s %-5s %-6s\n", "service", "instance", "node", "port");
	for (;;) {
		struct pollfd p;
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;
		/* 超时即认为列完了：内核【通常】会补一条全零的终止包，
		 * 但不能只靠它，否则服务表为空时会永久阻塞。 */
		if (poll(&p, 1, 1500) <= 0)
			break;
		if (recv(fd, &pkt, sizeof(pkt), 0) < (ssize_t)sizeof(pkt))
			continue;
		if (pkt.cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		if (!pkt.server.service && !pkt.server.instance &&
		    !pkt.server.node && !pkt.server.port)
			break;                  /* 全零 = 终止标记 */
		printf("%-8u %-9u %-5u %-6u\n",
		       pkt.server.service, pkt.server.instance,
		       pkt.server.node, pkt.server.port);
		found++;
	}
	printf("--- 共 %d 个服务\n", found);
	close(fd);
	return found ? 0 : 2;
}
