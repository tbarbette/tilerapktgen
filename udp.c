#include "generator.h"
#include "udp.h"
#include <stdio.h>
#include <stdlib.h>



void* rx_body(void* arg)
{
	int result;

	struct threadinfo* t = (struct threadinfo*)arg;

	int rank = t->rank;

	// Bind to a single cpu.
	result = tmc_cpus_set_my_cpu(tmc_cpus_find_nth_cpu(&cpus, rank));
	VERIFY(result, "tmc_cpus_set_my_cpu()");

	mlockall(MCL_CURRENT);
	tmc_sync_barrier_wait(&sync_barrier);
	tmc_spin_barrier_wait(&spin_barrier);

	gxio_mpipe_iqueue_t* iqueue = iqueues[rank];

	while (!rxterminate)
	{

		gxio_mpipe_idesc_t* idescs;

		int n = gxio_mpipe_iqueue_try_peek(iqueue, &idescs);

		if (n <= 0)
		{
			if (rxterminate)
				break;
			continue;

		}

		tmc_mem_prefetch(idescs, n * sizeof(*idescs));

		for (int i = 0; i < n; i++)
		{
			gxio_mpipe_idesc_t* idesc = &idescs[i];

			if (idesc->be || idesc->me || idesc->tr || idesc->ce)
				printf("Packet error %d %d %d %d!\n",idesc->be , idesc->me , idesc->tr , idesc->ce);

		}

		// Consume the packets.
		for (int i = 0; i < n; i++) {
			gxio_mpipe_idesc_t* idesc = &idescs[i];
			gxio_mpipe_iqueue_drop(iqueue, idesc);
			gxio_mpipe_iqueue_consume(iqueue,idesc);
		}

		t->rxbytes += pktlen;
		t->rxcount += n;
	}


	return (void*)NULL;
}

/*
 * Initlialize a packet
 */

/* Compute the checksum of the given data. */
uint16_t checksum(const void *data, uint16_t len, uint32_t sum)
{
	const uint8_t *addr = data;
	uint32_t i;

	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	return sum;
}

void initialize_packet(struct ether_addr src, struct ether_addr dst, struct pkt* pkt, int adddr)
{
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *udp;

	uint16_t paylen = pktlen - sizeof(*eh) - sizeof(struct ip);

	for (int i = 0; i < paylen/4; i++) {
		*(((int*)pkt->body) + i) = rand();
	}
	pkt->body[paylen] = '\0';

	ip = &pkt->ip;

	ip->ip_v = IPVERSION;
	ip->ip_hl = 5;
	ip->ip_id = 0;
	ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = ntohs(pktlen - sizeof(*eh));
	ip->ip_id = 0;
	ip->ip_off = htons(IP_DF); /* Don't fragment */
	ip->ip_ttl = IPDEFTTL;
	ip->ip_p = IPPROTO_UDP;

	ip->ip_dst.s_addr = adddr;
	ip->ip_src.s_addr = 0x0100010a;
	ip->ip_sum = 0;
	ip->ip_sum = htons(~(checksum(ip,sizeof(struct ip), 0)) & 0xFFFF);

	udp = &pkt->udp;
	udp->source = htons(2048);
	udp->dest = htons(4096);
	udp->len = htons(paylen);

	udp->check =  htons(~(checksum(udp, sizeof(*udp),
			checksum(pkt->body,
					paylen - sizeof(*udp),
					checksum(&ip->ip_src, 2 * sizeof(ip->ip_src),
							IPPROTO_UDP + (u_int32_t)ntohs(udp->len)
					)
			)
	)) & 0xFFFF);

	eh = &pkt->eh;
	bcopy(&src, eh->ether_shost, 6);
	bcopy(&dst, eh->ether_dhost, 6);
	eh->ether_type = htons(ETHERTYPE_IP);
}


void*
tx_body(void* arg)
{
	int result;

	struct threadinfo* t = (struct threadinfo*)arg;

	int rank = t->rank;

	int device = t->device;

	// Bind to a single cpu.
	result = tmc_cpus_set_my_cpu(tmc_cpus_find_nth_cpu(&cpus,num_workers + rank));
	VERIFY(result, "tmc_cpus_set_my_cpu()");

	mlockall(MCL_CURRENT);
	tmc_sync_barrier_wait(&sync_barrier);
	tmc_spin_barrier_wait(&spin_barrier);


	long slot = 0;

	void* pkt = t->pkt;
	void* end_pkt = t->pkt + (pkt_buffer_size * (num_genpackets / num_workers));

	uint16_t size = pktlen;
	gxio_mpipe_edesc_t edesc = {{
			.bound = 1,
			.xfer_size = size,

			.stack_idx = out_stack,
			.size = GXIO_MPIPE_BUFFER_SIZE_4096
	}};

	int n = 0;

	while (!txterminate || n != 0)
	{

		// printf("Sending packet\n");


		edesc.va = (long)(pkt);
		// Prepare to egress the packet.


		// Reserve slots in batches of 128 (for efficiency).  This will
		// block, since we blast faster than the hardware can egress.
		if (n == 0) {
			n = 128;
			slot = gxio_mpipe_equeue_reserve_fast(&equeues[device], n);
		}


		gxio_mpipe_equeue_put_at(&equeues[device], edesc, slot++);

		pkt += pkt_buffer_size;
		if (pkt >= end_pkt) pkt = t->pkt;
		n--;

		t->txcount += 1;
		t->txbytes += pktlen;

		if (pause_time)
			usleep(pause_time);
	}

	return (void*)NULL;
}


void udp_genpackets(void* mem) {
	struct pkt* pkt = (struct pkt*)mem;

	u_int8_t srcd[6] = {rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256}; //Src mac

	for (int i = 0; i < num_genpackets; i++) {
		pkt = mem + (pkt_buffer_size*i);
		struct ether_addr src, dst;
		if (randomdst) {
			u_int8_t srcr[6] = {rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256}; //Src mac
			bcopy(srcr,&src,  6);
		} else
			bcopy(srcd,&src,  6);

		u_int8_t dstd[6] = {0xff,0xff,0xff,0xff,0xff,0xff}; //Dst mac
		bcopy(srcd,&src,  6);
		bcopy(dstd,&dst,  6);

		unsigned long dstip =  ((0xa0) << 24) | (0x00000000 + (i * 29));
		dstip = (dstip & 0xffffff00ul) + ((dstip & 0xff) % 254 + 1);
		if (randomdst)
			initialize_packet(src,dst,pkt,htonl(dstip));
		else
			initialize_packet(src,dst,pkt,htonl(((0xa0) << 24) + 1));

	}
}
