#ifndef _TILERA_GEN_H_
#define _TILERA_GEN_H_

#include <pthread.h>


#include <sys/mman.h>
#include <sys/dataplane.h>


#include <tmc/alloc.h>

#include <arch/atomic.h>
#include <arch/sim.h>

#include <gxio/mpipe.h>

#include <tmc/cpus.h>
#include <tmc/mem.h>
#include <tmc/spin.h>
#include <tmc/sync.h>
#include <tmc/task.h>

// The initial affinity.
cpu_set_t cpus;

// The ingress queues (one per worker).
gxio_mpipe_iqueue_t** iqueues;

// Help synchronize thread creation.
tmc_sync_barrier_t sync_barrier;
tmc_spin_barrier_t spin_barrier;

volatile int txterminate;
volatile int rxterminate;

int pktlen;

int num_genpackets;

int pkt_buffer_size;

int out_stack;

//Randomize header destination
int randomdst;

//Time to wait after a packet has been sent
int pause_time;

// The egress queue (1 per device, shared by all workers on the same device).
gxio_mpipe_equeue_t* equeues;

// The number of workers to use.
unsigned int num_workers;


// Align "p" mod "align", assuming "p" is a "void*".
#define ALIGN(p, align) do { (p) += -(long)(p) & ((align) - 1); } while(0)

// Help check for errors.
#define VERIFY(VAL, WHAT)                                       \
		do {                                                          \
			long long __val = (VAL);                                    \
			if (__val < 0)                                              \
			tmc_task_die("Failure in '%s': %lld: %s.",                \
					(WHAT), __val, gxio_strerror(__val));        \
		} while (0)


struct threadinfo {
	long int txcount,rxcount,txbytes,rxbytes;
	pthread_t rxthread,txthread;
	void* pkt;
	int rank;
	int device;
};

#endif
