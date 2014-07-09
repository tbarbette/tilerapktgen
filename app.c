/****************************************************************
 *
 * The author of this software is Tom Barbette
 *
 * Copyright (c) 2014 by University of Liege
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that the name of the
 * author is included in all copies of any software which is or includes
 * copy or modification of this software and in all copies of the
 * supporting documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR LUCENT MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * This software is based on Tilera MDE samples applications.
 *
 ***************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "generator.h"
#include "udp.h"

int max_pktlen = 1501;

char* prefix = "";

int ndevice = 0;

int init_time = 2;

int send_time = 10;

int pktlen = 60;

int num_genpackets = 256 * 8;

int pkt_buffer_size = 4096;

int randomdst = 1;

int pause_time = 0;

int verbose = 0;

unsigned int num_workers = 1;

volatile int txterminate;
volatile int rxterminate;

// The flag to indicate packet forward is done.
static volatile bool done = false;

// The number of entries in the equeue ring - 2K (2048).
static unsigned int equeue_entries = GXIO_MPIPE_EQUEUE_ENTRY_2K;

// The mPIPE instance.
static int instance;

// The mpipe context (shared by all workers).
static gxio_mpipe_context_t context_body;
static gxio_mpipe_context_t* const context = &context_body;

// The total number of packets forwarded by all workers.
// Reserve a cacheline for "total" to eliminate the false sharing.
#define total total64.v
struct {
	volatile unsigned long v __attribute__ ((aligned(CHIP_L2_LINE_SIZE())));
} total64 = { 0 };




// Allocate memory for a buffer stack and its buffers, initialize the
// stack, and push buffers onto it.
static void
create_stack(gxio_mpipe_context_t* context, int stack_idx,
		gxio_mpipe_buffer_size_enum_t buf_size, int num_buffers)
{
	int result;

	// Extract the actual buffer size from the enum.
	size_t size = gxio_mpipe_buffer_size_enum_to_buffer_size(buf_size);

	// Compute the total bytes needed for the stack itself.
	size_t stack_bytes = gxio_mpipe_calc_buffer_stack_bytes(num_buffers);

	// Round up so that the buffers will be properly aligned.
	stack_bytes += -(long)stack_bytes & (128 - 1);

	// Compute the total bytes needed for the stack plus the buffers.
	size_t needed = stack_bytes + num_buffers * size;

	// Allocate up to 16 pages of the smallest suitable pagesize.
	tmc_alloc_t alloc = TMC_ALLOC_INIT;
	tmc_alloc_set_pagesize(&alloc, needed / 16);
	size_t pagesize = tmc_alloc_get_pagesize(&alloc);
	int pages = (needed + pagesize - 1) / pagesize;
	void* mem = tmc_alloc_map(&alloc, pages * pagesize);
	if (mem == NULL)
		tmc_task_die("Could not allocate buffer pages.");

	// Initialize the buffer stack.
	result = gxio_mpipe_init_buffer_stack(context, stack_idx, buf_size,
			mem, stack_bytes, 0);
	VERIFY(result, "gxio_mpipe_init_buffer_stack()");

	// Register the buffer pages.
	for (int i = 0; i < pages; i++)
	{
		result = gxio_mpipe_register_page(context, stack_idx,
				mem + i * pagesize, pagesize, 0);
		VERIFY(result, "gxio_mpipe_register_page()");
	}

	// Push the actual buffers.
	mem += stack_bytes;
	for (int i = 0; i < num_buffers; i++)
	{
		gxio_mpipe_push_buffer(context, stack_idx, mem);
		mem += size;
	}
}







/*
typedef enum {GEN_UDP,GEN_HTTP} gentype_t;
gentype_t gentype = GEN_HTTP;
*/



void print_help() {
	printf("Usage : generator [OPTION] --link xgbe1,xgbe2,...\n"
			"	--init_time Time in usec before starting counting packets sent. Default is 2.\n"
			"   --send_time Time in usec of the benchmark. Defautl is 10.\n"
			"   -t Prefix to print before each result line.\n"
			"	-b Start packet length. Default is 60.\n"
			"	-l End packet length. Default is 1500.\n"
			"	-w Number of threads for receive side and sending side. (Total is twice this number).\n"
			"	-r Pause time in usec after each packet sent. Default is 0.\n"
			"	-s 0/1 Use random destinsation. Default is 1 (true).\n"
			"	-h Print this help message.\n");
}


int main(int argc, char** argv) {


	char* title = "DEFAULT";
	int receive_pause = 0;

	char* links = NULL;

	// Parse args.
	for (int i = 1; i < argc; i++)
	{
		char* arg = argv[i];

		// --link <link_name>, link_name is for both ingress and egress.
		if (!strcmp(arg, "--link") && i + 1 < argc)
		{
			char * link = argv[++i];
			links = link;
			int count = 1;
			while (*link != '\0') {
				if (*link == ',') count++;
				link++;
			}
			ndevice = count;

		}
		else if (!strcmp(arg, "--init_time") && i + 1 < argc)
		{
			init_time = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "--send_time") && i + 1 < argc)
		{
			send_time = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-t") && i + 1 < argc)
		{
			title = argv[++i];
		}
		else if (!strcmp(arg, "-b") && i + 1 < argc)
		{
			pktlen = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-l") && i + 1 < argc)
		{
			max_pktlen = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-w") && i + 1 < argc)
		{
			num_workers = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-r") && i + 1 < argc)
		{
			pause_time = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-s") && i + 1 < argc)
		{
			randomdst = atoi(argv[++i]);
		}
		else if (!strcmp(arg, "-h"))
		{
			print_help();
			return 0;
		}
		else
		{
			print_help();
			tmc_task_die("Unknown option '%s'.", arg);
		}
	}

	if (ndevice <= 0) {
		tmc_task_die("You have to provide some links");
	}

	if (num_workers < ndevice) {
		tmc_task_die("More devices than workers?");
	}

	if ((num_workers % ndevice) != 0) {
		tmc_task_die("Workers must be a multiple of ndevice");
	}

	int result;

	// Determine the available cpus.
	result = tmc_cpus_get_my_affinity(&cpus);
	VERIFY(result, "tmc_cpus_get_my_affinity()");

	if (tmc_cpus_count(&cpus) < (num_workers * 2))
		tmc_task_die("Insufficient cpus.");

	int channels[ndevice];

	for (int idevice = 0; idevice < ndevice; idevice ++) {
		char link_name[100];
		int i = 0;
		while (links[i] != '\0' && links[i] != ',') {
			link_name[i] = links[i];
			i++;
		}
		link_name[i] = '\0';
		links+=i + 1;

		if (verbose)
			printf("Allocating link %s\n",link_name);


		if (idevice == 0) {
			// Get the instance.
			instance = gxio_mpipe_link_instance(link_name);
			if (instance < 0)
				tmc_task_die("Link '%s' does not exist.", link_name);

			// Start the driver.
			result = gxio_mpipe_init(context, instance);
			VERIFY(result, "gxio_mpipe_init()");
		}

		gxio_mpipe_link_t link;
		result = gxio_mpipe_link_open(&link, context, link_name, GXIO_MPIPE_LINK_CTL);
		VERIFY(result, "gxio_mpipe_link_open()");


		result = gxio_mpipe_link_set_attr(&link,GXIO_MPIPE_LINK_RECEIVE_PAUSE, receive_pause);
		VERIFY(result, "gxio_mpipe_link_set_attr()");


		channels[idevice] = gxio_mpipe_link_channel(&link);

	}

	// Allocate some iqueues.
	iqueues = calloc(num_workers, sizeof(*iqueues));
	if (iqueues == NULL)
		tmc_task_die("Failure in 'calloc()'.");

	// Allocate some NotifRings.
	result = gxio_mpipe_alloc_notif_rings(context, num_workers, 0, 0);
	VERIFY(result, "gxio_mpipe_alloc_notif_rings()");
	unsigned int ring = result;

	// Init the NotifRings.
	size_t notif_ring_entries = 512;
	size_t notif_ring_size = notif_ring_entries * sizeof(gxio_mpipe_idesc_t);
	size_t needed = notif_ring_size + sizeof(gxio_mpipe_iqueue_t);
	for (int i = 0; i < num_workers; i++)
	{
		tmc_alloc_t alloc = TMC_ALLOC_INIT;
		tmc_alloc_set_home(&alloc, tmc_cpus_find_nth_cpu(&cpus, i));
		// The ring must use physically contiguous memory, but the iqueue
		// can span pages, so we use "notif_ring_size", not "needed".
		tmc_alloc_set_pagesize(&alloc, notif_ring_size);
		void* iqueue_mem = tmc_alloc_map(&alloc, needed);
		if (iqueue_mem == NULL)
			tmc_task_die("Failure in 'tmc_alloc_map()'.");
		gxio_mpipe_iqueue_t* iqueue = iqueue_mem + notif_ring_size;
		result = gxio_mpipe_iqueue_init(iqueue, context, ring + i,
				iqueue_mem, notif_ring_size, 0);
		VERIFY(result, "gxio_mpipe_iqueue_init()");
		iqueues[i] = iqueue;
	}


	// Allocate a NotifGroup.
	result = gxio_mpipe_alloc_notif_groups(context, 1, 0, 0);
	VERIFY(result, "gxio_mpipe_alloc_notif_groups()");
	int group = result;

	// Allocate some buckets. The default mPipe classifier requires
	// the number of buckets to be a power of two (maximum of 4096).
	int num_buckets = 2048;
	result = gxio_mpipe_alloc_buckets(context, num_buckets, 0, 0);
	VERIFY(result, "gxio_mpipe_alloc_buckets()");
	int bucket = result;

	// Init group and buckets, preserving packet order among flows.
	gxio_mpipe_bucket_mode_t mode = GXIO_MPIPE_BUCKET_ROUND_ROBIN;
	result = gxio_mpipe_init_notif_group_and_buckets(context, group,
			ring, num_workers,
			bucket, num_buckets, mode);
	VERIFY(result, "gxio_mpipe_init_notif_group_and_buckets()");

	result = gxio_mpipe_alloc_edma_rings(context, ndevice, 0, 0);
	VERIFY(result, "gxio_mpipe_alloc_edma_rings");
	uint ering_start = result;


	// Allocate some iqueues.
	equeues = calloc(ndevice, sizeof(*equeues));
	if (equeues == NULL)
		tmc_task_die("Failure in 'calloc()'.");

	// Initialize the equeues.
	for (int idevice = 0; idevice < ndevice; idevice++) {

		if (verbose)
			printf("Allocating equeue %d for channel %d\n",idevice,channels[idevice]);
		size_t edescs_size = equeue_entries * sizeof(gxio_mpipe_edesc_t);
		tmc_alloc_t edescs_alloc = TMC_ALLOC_INIT;
		tmc_alloc_set_pagesize(&edescs_alloc, edescs_size);
		void* edescs = tmc_alloc_map(&edescs_alloc, edescs_size);
		if (edescs == NULL)
			tmc_task_die("Failed to allocate equeue memory.");
		result = gxio_mpipe_equeue_init(&equeues[idevice], context, ering_start + idevice, channels[idevice],
				edescs, edescs_size, 0);

		VERIFY(result, "gxio_gxio_equeue_init()");
	}


	// Use enough small/large buffers to avoid ever getting "idesc->be".
	unsigned int num_bufs = equeue_entries + num_workers * notif_ring_entries ;

	// Allocate small/large buffer stacks.
	result = gxio_mpipe_alloc_buffer_stacks(context, 3, 0, 0);
	VERIFY(result, "gxio_mpipe_alloc_buffer_stacks()");
	int stack_idx = result;

	// Initialize small/large stacks.
	create_stack(context, stack_idx + 0, GXIO_MPIPE_BUFFER_SIZE_128, num_bufs);
	create_stack(context, stack_idx + 1, GXIO_MPIPE_BUFFER_SIZE_1664, num_bufs);

	out_stack =  stack_idx + 2;

	size_t space;

	space = pkt_buffer_size * num_genpackets;

	tmc_alloc_t alloc = TMC_ALLOC_INIT;
	tmc_alloc_set_pagesize(&alloc, (space + 15) / 16);
	void* mem = tmc_alloc_map(&alloc, space);
	if (mem == NULL)
		tmc_task_die("Failed to allocate memory for %zd packet bytes.", space);

	// Register the pages.
	size_t pagesize = tmc_alloc_get_pagesize(&alloc);
	size_t pages = (space + pagesize - 1) / pagesize;
	for (int i = 0; i < pages; i++)
	{
		if (verbose)
			printf("Register %d, %p, %zd\n", out_stack,mem + i * pagesize, pagesize);
		int result =

				gxio_mpipe_register_page(context,  out_stack,
						mem + i * pagesize, pagesize, 0);
		VERIFY(result, "gxio_mpipe_register_page()");
	}


	// Register for packets.
	gxio_mpipe_rules_t rules;
	gxio_mpipe_rules_init(&rules, context);
	gxio_mpipe_rules_begin(&rules, bucket, num_buckets, NULL);
	result = gxio_mpipe_rules_commit(&rules);
	VERIFY(result, "gxio_mpipe_rules_commit()");


	sim_enable_mpipe_links(instance, -1);
	sleep(2);

	while (pktlen < max_pktlen) {
		usleep(5000);
		txterminate = 0;
		rxterminate = 0;
		__insn_mf();

		udp_genpackets(mem);

		tmc_sync_barrier_init(&sync_barrier, num_workers*2);
		tmc_spin_barrier_init(&spin_barrier, num_workers*2);

		struct threadinfo targs[num_workers];


		int i;

		struct timeval tv;
		long int txcount = 0,rxcount = 0,txbytes=0,rxbytes=0;
		unsigned long time;

		int pkt_count = num_genpackets / num_workers;
		int threads_per_device = num_workers / ndevice;

		for (int i = 0; i < num_workers; i++)
		{
			targs[i].rxcount = 0;
			targs[i].txcount = 0;
			targs[i].txbytes = 0;
			targs[i].rxbytes = 0;
			targs[i].rank = i;
			targs[i].pkt = mem + (pkt_buffer_size * (i * pkt_count));
			targs[i].device = i / threads_per_device;
			if (pthread_create(&(targs[i].rxthread), NULL, rx_body, &targs[i]) != 0)
				tmc_task_die("Failure in 'pthread_create()'.");
			if (pthread_create(&(targs[i].txthread), NULL, tx_body, &targs[i]) != 0)
				tmc_task_die("Failure in 'pthread_create()'.");
		}

		sim_enable_mpipe_links(instance, -1);

		//printf("All thread started. Starting launch in 5 seconds...\n");
		sleep(init_time);
		gettimeofday(&tv,NULL);
		time = 1000000 * tv.tv_sec + tv.tv_usec;

		for (i = 0; i < num_workers; i++) {
			txbytes += targs[i].txbytes;
			rxbytes += targs[i].rxbytes;
			txcount += targs[i].txcount;
			rxcount += targs[i].rxcount;
		}

		sleep(10);
		txterminate = 1;
		__insn_mf();
		gettimeofday(&tv,NULL);

		unsigned long ftime = 1000000 * tv.tv_sec + tv.tv_usec;
		long int ftxcount = 0,frxcount = 0,ftxbytes=0,frxbytes=0;
		for (i = 0; i < num_workers; i++) {
			ftxbytes += targs[i].txbytes;
			frxbytes += targs[i].rxbytes;
			ftxcount += targs[i].txcount;
			frxcount += targs[i].rxcount;
		}

		double dtime = (double)(ftime-time);
		double txdiff = (double)(ftxbytes-txbytes);
		double rxdiff = (double)(frxbytes-rxbytes);
		double txrate = txdiff/dtime;
		double rxrate = rxdiff/dtime;
		double lossrate =  (txdiff - rxdiff)/txdiff;
		double txspeed = 8 * (txdiff) / dtime;
		double rxspeed = 8 * (rxdiff) / dtime;
		double wtxspeed = 8 * (txdiff + (txcount* 24)) / dtime;
		double wrxspeed = 8 * (rxdiff + (rxcount* 24)) / dtime;
		printf("%s %d %lu %lu %lu %.4lf %.4lf %.2lf %.2lf %.2lf %.2lf %.2lf\n",title, pktlen, ftime-time,ftxcount-txcount,frxcount-rxcount,txrate,rxrate,txspeed,rxspeed,wtxspeed,wrxspeed,lossrate);


		usleep(100);
		rxterminate = 1;
		__insn_mf();
		for (int i = 0; i < num_workers; i++)
		{
			if (pthread_join(targs[i].rxthread, NULL) != 0)
				tmc_task_die("Failure in 'pthread_join()'.");
			if (pthread_join(targs[i].txthread, NULL) != 0)
				tmc_task_die("Failure in 'pthread_join()'.");
		}

		pktlen ++;
	}

	//error:
	gxio_mpipe_destroy(context);
	return 0;
}
