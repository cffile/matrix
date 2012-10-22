#include <types.h>
#include <stddef.h>
#include <string.h>
#include "matrix/debug.h"
#include "matrix/const.h"
#include "hal/hal.h"
#include "hal/cpu.h"
#include "hal/spinlock.h"
#include "mm/malloc.h"
#include "sys/time.h"
#include "timer.h"
#include "proc/process.h"
#include "proc/sched.h"

/* Number of priority levels */
#define NR_PRIORITIES	32

/* Time quantum to give to threads */
#define THREAD_QUANTUM

/* Run queue structure */
struct sched_queue {
	uint32_t bitmap;			// Bitmap of queues with data
	struct list threads[NR_PRIORITIES];	// Queues of runnable threads
};
typedef struct sched_queue sched_queue_t;

/* Per-CPU scheduling information structure */
struct sched_cpu {
	struct spinlock lock;			// Lock to protect information/queues
	
	struct thread *prev_thread;		// Previously executed thread
	struct thread *idle_thread;		// Thread scheduled when no other threads runnable

	struct timer timer;			// Preemption timer
	struct sched_queue *active;		// Active queue
	struct sched_queue *expired;		// Expired queue
	struct sched_queue queues[2];		// Active and expired queues
	
	size_t total;				// Total running/ready thread count
};
typedef struct sched_cpu sched_cpu_t;

/* The schedule queue for our kernel */
struct process *_ready_head[NR_SCHED_QUEUES];
struct process *_ready_tail[NR_SCHED_QUEUES];

/* Pointer to our various task */
struct process *_prev_proc = NULL;
struct process *_curr_proc = NULL;		// Current running process

static struct cpu *sched_alloc_cpu(struct thread *t)
{
	struct cpu *cpu, *other;

	/* On UP systems, the only choice is current CPU */
	if (_nr_cpus == 1) {
		return CURR_CPU;
	}

	cpu = NULL;
	
	return cpu;
}

static void sched_adjust_priority(struct sched_cpu *c, struct process *p)
{
	;
}

/**
 * Pick a new process to run
 */
static struct process *sched_pick_process(struct sched_cpu *c)
{
	struct process *p;
	int q;

	/* Check each of the scheduling queues for ready processes. The number of
	 * queues is defined in process.h. The lowest queue contains IDLE, which
	 * is always ready.
	 */
	for (q = 0; q < NR_SCHED_QUEUES; q++) {
		if ((p = _ready_head[q]) != NULL) {
			break;
		}
	}

	return p;
}

/**
 * Add `p' to one of the queues of runnable processes. This function is responsible
 * for inserting a process into one of the scheduling queues. `p' must not in the
 * scheduling queues before enqueue.
 */
void sched_enqueue(struct process *p)
{
	int q;
	struct sched_cpu *c;

#ifdef _DEBUG_SCHED
	check_runqueues("sched_enqueue:begin");
#endif
	
	/* Determine where to insert the process */
	q = p->priority;
	
	/* Now add the process to the queue. */
	if (_ready_head[q] == NULL) {			// Add to empty queue
		_ready_head[q] = _ready_tail[q] = p;
		p->next = NULL;
	} else {					// Add to tail of queue
		_ready_tail[q]->next = p;
		_ready_tail[q] = p;
		p->next = NULL;
	}

#ifdef _DEBUG_SCHED
	check_runqueues("sched_enqueue:end");
#endif
}

/**
 * A process must be removed from the scheduling queues, for example, because it has
 * been blocked.
 */
void sched_dequeue(struct process *p)
{
	int q;
	boolean_t proc_found = FALSE;
	struct process **xpp;
	struct process *prev_ptr;
	struct sched_cpu *c;

	q = p->priority;

#ifdef _DEBUG_SCHED
	check_runqueues("sched_dequeue:begin");
#endif

	/* Now make sure that the process is not in its ready queue. Remove the process
	 * if it was found.
	 */
	prev_ptr = NULL;
	for (xpp = &_ready_head[q]; *xpp != NULL; xpp = &((*xpp)->next)) {
		
		if (*xpp == p) {			// Found process to remove
			*xpp = (*xpp)->next;		// Replace with the next in chain
			if (p == _ready_tail[q]) {	// Queue tail removed
				_ready_tail[q] = prev_ptr; // Set new tail
			} else if (p == _ready_head[q]) {  // Queue head removed
				_ready_head[q] = *xpp;
			}
			proc_found = TRUE;
			break;				// Break out the for loop
		}
		prev_ptr = *xpp;
	}

	ASSERT(proc_found == TRUE);			// The process should be found

#ifdef _DEBUG_SCHED
	check_runqueues("sched_dequeue:end");
#endif
}

/**
 * Picks a new process to run and switches to it. Interrupts must be disable.
 * @param state		- Previous interrupt state
 */
void sched_reschedule(boolean_t state)
{
	struct sched_cpu *c;
	struct process *next;

	c = CURR_CPU->sched;

	/* Adjust the priority of the thread based on whether it used up its quantum */
	sched_adjust_priority(c, CURR_PROC);

	/* Enqueue and dequeue the current process to update the process queue */
	sched_dequeue(CURR_PROC);
	sched_enqueue(CURR_PROC);
	
	/* Find a new process to run. A NULL return value means no processes are
	 * ready, so we schedule the idle process in this case.
	 */
	next = sched_pick_process(c);
	ASSERT(next != NULL);
	next->quantum = P_QUANTUM;

	/* Move the next process to running state and set it as the current */
	_prev_proc = CURR_PROC;
	next->state = PROCESS_RUNNING;
	CURR_PROC = next;

	/* Perform the process switch if current process is not the same as previous
	 * one.
	 */
	if (CURR_PROC != _prev_proc) {
		process_switch(CURR_PROC, _prev_proc);
		irq_restore(state);
	} else {
		irq_restore(state);
	}
}

void init_sched_percpu()
{
	int i, j;

	/* Initialize the scheduler for the current CPU */
	CURR_CPU->sched = kmalloc(sizeof(struct sched_cpu), 0);
	CURR_CPU->sched->total = 0;
	CURR_CPU->sched->active = &CURR_CPU->sched->queues[0];
	CURR_CPU->sched->expired = &CURR_CPU->sched->queues[1];

	/* Create the preemption timer */
	init_timer(&CURR_CPU->sched->timer);

	/* Initialize queues */
	for (i = 0; i < 2; i++) {
		CURR_CPU->sched->queues[i].bitmap = 0;
		for (j = 0; j < NR_PRIORITIES; j++) {
			LIST_INIT(&CURR_CPU->sched->queues[i].threads[j]);
		}
	}
}

void init_sched()
{
	memset(&_ready_head[0], 0, sizeof(struct process *) * NR_SCHED_QUEUES);
	memset(&_ready_tail[0], 0, sizeof(struct process *) * NR_SCHED_QUEUES);

	/* Enqueue the kernel process which is the first process in our system */
	sched_enqueue(_kernel_proc);
	
	/* At this time we can schedule the process now */
	CURR_PROC = _kernel_proc;

	/* Switch to current process, we set previous process to CURR_PROC so we
	 * the context can get initialized.
	 */
	process_switch(CURR_PROC, CURR_PROC);
}
