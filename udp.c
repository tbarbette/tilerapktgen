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
			uint32_t* buf;
			unsigned int len;
			if (idesc->be || idesc->me || idesc->tr || idesc->ce)
				printf("Packet error %d %d %d %d!\n",idesc->be , idesc->me , idesc->tr , idesc->ce);

			if (sendtime && gxio_mpipe_idesc_get_l2_length(idesc) != pktlen) {
				printf("Packet size is %d instead of %d !",gxio_mpipe_idesc_get_l2_length(idesc), pktlen);
			}
			// Consume the packets.

			/*buf = (uint32_t*)((uint8_t*)gxio_mpipe_idesc_get_l4_start(idesc) + 8); //End of UDP
			len = gxio_mpipe_idesc_get_l4_length(idesc) -8;


			//Check content
			for (int j = 0; j < len / 4; j++) {
				if (buf[j] != j)
					printf("Content  %d/%d : %x\n", j,len/4,buf[j]);
			}*/
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

void initialize_packet(struct ether_addr src, struct ether_addr dst, struct pkt* pkt, uint32_t src_addr, uint32_t dst_addr)
{
	struct ether_header *eh;
	struct ip *ip;
	struct udphdr *udp;

	uint16_t paylen = pktlen - sizeof(*eh) - sizeof(struct ip);

	for (int i = 0; i < paylen/4; i++) {
		*(((int*)pkt->body) + i) = i;
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

	ip->ip_dst.s_addr = dst_addr;
	ip->ip_src.s_addr = src_addr;
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


void udp_genpackets(unsigned char* mem) {

	struct pkt* pkt = (struct pkt*)mem;

	u_int8_t srcd[6] = {rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256}; //Src mac

	uint32_t dst_a = ((0x0a) << 24);
	unsigned int router_b = 0;

	uint32_t src_addr;
	uint32_t dst_addr;


	int last_interface = -1;

	int packet_per_if = num_genpackets / ndevice;
	int entry_per_if = 0;

	int destinations[packet_per_if];
	int number[packet_per_if];

	if (dsttype == DST_ROUTER) {
		int count = 0;
		int num[ndevice - 1];
		for (int i =0; i<ndevice - 1;i++) num[i] = 0;

		do {

			int min = 65536;
			int min_int = 0;
			for (int i = 0; i < ndevice - 1; i++) {
				if (num[i] < min) {
					min = num[i];
					min_int = i % (ndevice - 1);
				}
			}

			destinations[entry_per_if] = min_int;

			if (flow_max > 1) {
				double prob;
				int gen;

				//High probability of little flows, very low of middle flows, and average probability of long flow
				do {
				gen = rand();
				double f = (double)gen / ((double)(RAND_MAX)/1.6) - 1.0; //NUmber between -1 and 0.6
				prob = f * f * f * f * f * f * f * f * 0.99 + 0.01f; // number distributed between 0 and 1

				//SHould we keep this number?
				} while ((double)rand() / (double)RAND_MAX > prob);
				number[entry_per_if] = gen / (RAND_MAX / flow_max) + 1;
			} else {
				number[entry_per_if] = 1;
			}

			num[min_int] += number[entry_per_if];
			count += number[entry_per_if];

			entry_per_if++;
		} while (entry_per_if < packet_per_if && count < packet_per_if);
		number[entry_per_if - 1] -= count - packet_per_if;
		num[destinations[entry_per_if - 1]] -= count - packet_per_if;
		//printf("Num : %d %d %d\n",num[0],num[1],num[2]);
		int tocorrect[ndevice - 1];
		for (int i =0; i< ndevice - 1;i++) {
			tocorrect[i] = (packet_per_if / (ndevice - 1)) + 1 - num[i];
		}
		//printf("Corr : %d %d %d\n",tocorrect[0],tocorrect[1],tocorrect[2]);
		for (int i =0; i < entry_per_if; i++) {
			int d = destinations[i];
			if (tocorrect[d] == 0) continue;
			if (tocorrect[d] < 0 && number[i] > 1) {
				int new = number[i] + tocorrect[d];
				if (new < 1) new = 1;
				int diff = number[i] - new;
				tocorrect[d] += diff;
				number[i] = new;
				num[d] -= diff;

			} else if (tocorrect[d] > 0) {
				number[i] += tocorrect[d];
				num[d] += tocorrect[d];
				tocorrect[d] = 0;
			}

		}
		//printf("Num : %d %d %d\n",num[0],num[1],num[2]);

	}


	int i = 0;
	for (int interface = 0; interface < ndevice; interface++) {
		int entry = 0;
		int keepflow  = 0;
	/*	int num[ndevice];
		for (int i =0; i<ndevice;i++) num[i] = 0;*/

		for (int j = 0; j < packet_per_if; j++) {

			pkt = (struct pkt*)(mem + (pkt_buffer_size*i));

			struct ether_addr src, dst;
			if (dsttype == DST_RAND || dsttype == DST_ROUTER) {
				u_int8_t srcr[6] = {rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256,rand() % 256}; //Src mac
				bcopy(srcr,&src,  6);
			} else
				bcopy(srcd,&src,  6);

			bcopy(srcd,&src,  6);

			if (!dsts) {
				u_int8_t dstd[6] = {0xff,0xff,0xff,0xff,0xff,0xff}; //Dst mac
				bcopy(dstd, &dst,  6);
			} else {

				bcopy(&(dsts[interface * 6]), &dst,  6);
			}

			if (flow_max > 1 && keepflow) {
				keepflow--;
			} else {
				last_interface = interface;
				src_addr = 0x0a010001;
				dst_addr = dst_a + 1;
				if (dsttype == DST_RAND) {
					uint32_t dstip =  dst_a | (0x00000000 + (i * 29));
					dst_addr = (dstip & 0xffffff00ul) + ((dstip & 0xff) % 254 + 1);
				} else if (dsttype == DST_FIXE) {

				} else if (dsttype == DST_ROUTER) {

					//While the destination matches the interface

					router_b = destinations[entry];
					if (router_b >= interface) router_b++;
					keepflow = number[entry] - 1;
					entry++;

					/*do {
						router_b = (router_b + 1) % 4;
					} while (router_b == interface);*/


					uint32_t dst_b = (router_b + 1) << 16;

					uint32_t dst_cd = i * 29;
					dst_cd = (((dst_cd + 0x100) & 0xff00) | (((dst_cd & 0x00ff) % 254) + 1)) & 0xffff;
					uint32_t src_b = (interface + 1) << 16;
					src_addr = dst_a | src_b | dst_cd;
					dst_addr = dst_a | dst_b | dst_cd;

				}

				//printf("Flow of %d packets\n",keepflow);
			}
		//num[router_b]++;
		//printf("%u.%u.%u.%u ", src_addr >> 24 & 0x00ff, src_addr >> 16 & 0x00ff, src_addr >> 8 & 0x00ff,src_addr & 0x00ff);
		//printf("-> %u.%u.%u.%u\n", dst_addr >> 24 & 0x00ff, dst_addr >> 16 & 0x00ff, dst_addr >> 8 & 0x00ff,dst_addr & 0x00ff);
			initialize_packet(src,dst,pkt,htonl(src_addr),htonl(dst_addr));
			i++;
		}
		//printf("Num : %d %d %d %d\n",num[0],num[1],num[2],num[3]);
	}


}
