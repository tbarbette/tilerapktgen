#ifndef _TILERA_UDPGEN_H_
#define _TILERA_UDPGEN_H_

#include "generator.h"

#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sched.h>
#include <linux/ethtool.h>
#include <netinet/ip6.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>


struct pkt {
	struct ether_header eh;
	struct ip ip;
	struct udphdr udp;
	uint8_t body[2048];	// XXX hardwired
} __attribute__((__packed__));



void* rx_body(void* arg);
void initialize_packet(struct ether_addr src, struct ether_addr dst, struct pkt* pkt, uint32_t src_addr, uint32_t dst_addr);
void* tx_body(void* arg);
void udp_genpackets(unsigned char* mem);

#endif
