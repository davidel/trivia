/*    Copyright 2023 Davide Libenzi
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 * 
 */


#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>

#define ETH_PKT_SIZE 2000
#define ETH_PKT_READ 500

static int raw_pkt_write(const char *device, char *packet, int len) {
	int fd;
	struct ether_header *eptr;
	struct ifreq ifr;
	struct sockaddr_ll sll;
	unsigned char enet_src[6];

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd == -1) {
		perror("socket(PF_PACKET, SOCK_RAW)");
		return -1;
        }

	eptr = (struct ether_header *) packet;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
		perror(device);
		close(fd);
		return -2;
	}
	memcpy(enet_src, ifr.ifr_hwaddr.sa_data, 6);
	fprintf(stdout, "src address: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
		enet_src[0], enet_src[1], enet_src[2], enet_src[3], enet_src[4], enet_src[5]);

	memcpy(eptr->ether_shost, enet_src, 6);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1) {
		perror(device);
		close(fd);
		return -3;
	}
	fprintf(stdout, "iface index: %d\n", (int) ifr.ifr_ifindex);

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifr.ifr_ifindex;
	sll.sll_protocol = htons(ETH_P_ALL);
	if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) == -1) {
		perror("socket to interface bind");
		close(fd);
		return -4;
	}

	if (write(fd, packet, len) != len) {
		perror("packet write");
		close(fd);
		return -5;
	}
	close(fd);

	return 0;
}

static char *read_in_pkt(int fd, int *pktsize) {
	int size = 0, count = 0, rdb;
	char *pkt = NULL, *npkt;

	for (;;) {
		if (size - count < ETH_PKT_READ) {
			size = size + ETH_PKT_SIZE;
			if ((npkt = (char *) realloc(pkt, size)) == NULL) {
				perror("packet memory alloc");
				free(pkt);
				return NULL;
			}
			pkt = npkt;
		}
		if ((rdb = read(fd, pkt + count, ETH_PKT_READ)) < 0) {
			perror("packet read");
			free(pkt);
			return NULL;
		}
		count += rdb;
		if (rdb == 0)
			break;
	}
	*pktsize = count;

	return pkt;
}

static void usage(const char *prg) {

	fprintf(stderr, "use: %s -s SRCIF [-d DSTIF] [-t ETYPE] [-h]\n", prg);
}

int main(int ac, char **av) {
	int i, etype = -1, packet_size;
	const char *if_src = NULL, *if_dst = NULL;
	char *packet;
	struct ether_header *eptr;
	unsigned char enet_dst[6];

	while ((i = getopt(ac, av, "s:d:t:h")) != -1) {
		switch (i) {
		case 's':
			if_src = optarg;
			break;
		case 'd':
			if_dst = optarg;
			break;
		case 't':
			etype = atoi(optarg);
			break;
		case 'h':
			usage(av[0]);
			return 1;
		}
	}
	if (if_src == NULL) {
		usage(av[0]);
		return 1;
	}

	if ((packet = read_in_pkt(fileno(stdin), &packet_size)) == NULL)
		return 2;
	if (packet_size < sizeof(struct ether_header)) {
		fprintf(stderr, "ethernet packet size too small (%d)\n", packet_size);
		return 2;
	}
	fprintf(stdout, "packet size: %d\n", packet_size);

	eptr = (struct ether_header *) packet;

	if (etype >= 0)
		eptr->ether_type = htons(etype);
	fprintf(stdout, "eth type: %d\n", (int) ntohs(eptr->ether_type));

	if (if_dst != NULL) {
		if (sscanf(if_dst, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &enet_dst[0], &enet_dst[1],
			   &enet_dst[2], &enet_dst[3], &enet_dst[4], &enet_dst[5]) != 6) {
			fprintf(stderr, "invalid ethernet address (%s)\n", if_dst);
			return 2;
		}
		fprintf(stdout, "dst address: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
			enet_dst[0], enet_dst[1], enet_dst[2], enet_dst[3], enet_dst[4], enet_dst[5]);

		memcpy(eptr->ether_dhost, enet_dst, 6);
	}

	if (raw_pkt_write(if_src, packet, packet_size) < 0)
		return 2;

	return 0;
}

