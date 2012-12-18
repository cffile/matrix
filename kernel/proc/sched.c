#include <types.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "debug.h"
#include "matrix/const.h"
#include "hal/hal.h"
#include "hal/cpu.h"
#include "hal/spinlock.h"
#include "mm/malloc.h"
#include "mm/mmu.h"
#include "sys/time.h"
#include "timer.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "semaphore.h"

/* Number of priority levels */
#define NR_PRIORITIES	32

/* Time quantum to give to threads */
#define THREAD_QUANTUM	4000

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

/* Total number of running or ready threads across all CPUs */
static int _nr_running_threads = 0;

/* Dead process queue */
static struct list _dead_threads = {
	.prev = &_dead_threads,
	.next = &_dead_threads
};
static struct spinlock _dead_threads_lock;
static struct semaphore _dead_threads_sem;

/* Allocate a CPU for a thread to run on */
static struct cpu *sched_alloc_cpu(struct thread *t)
{
	size_t load, average, total;
	struct cpu *cpu, *other;
	struct list *l;

	/* On UP systems, the only choice is current CPU */
	if (_nr_cpus == 1) {
		return CURR_CPU;
	}

	/* Add 1 to the total number of threads to account for the thread we
	 * are adding.
	 */
	total = _nr_running_threads + 1;
	average = total / _nr_cpus;

	LIST_FOR_EACH(l, &_running_cpus) {
		other = LIST_ENTRY(l, struct cpu, link);
		load = other->sched->total;
		if (load < average) {
			cpu = other;
			break;
		}
	}
	
	return cpu;
}

/**
 * Add `p' to one of the queues of runnable processes. This function is responsible
 * for inserting a process into one of the scheduling queues. `p' must not in the
 * scheduling queues before enqueue.
 */
static void sched_enqueue(struct sched_queue *queue, struct thread *t)
{
	int q;
#ifdef _DEBUG_SCHED
	struct thread *thrd;
	struct list *l;
#endif	/* _DEBUG_SCHED */

	/* Determine where to insert the process */
	q = t->priority;

#ifdef _DEBUG_SCHED
	LIST_FOR_EACH(l, &queue->threads[q]) {
		thrd = LIST_ENTRY(l, struct thread, runq_link);
		if (thrd == t) {
			ASSERT(FALSE);
		}
	}
#endif	/* _DEBUG_SCHED */
	
	/* Now add the process to the tail of the queue. */
	ASSERT((q < NR_PRIORITIES) && (q >= 0));
	list_add_tail(&t->runq_link, &queue->threads[q]);
}

/**
 * A process must be removed from the scheduling queues, for example, because it has
 * been blocked.
 */
static void sched_dequeue(struct sched_queue *queue, struct thread *t)
{
	int q;
#ifdef _DEBUG_SCHED
	size_t found_times = 0;
	struct thread *thrd;
	struct list *l;
#endif	/* _DEBUG_SCHED */

	q = t->priority;

	/* Now make sure that the process is not in its ready queue. Remove the process
	 * if it was found.
	 */
	ASSERT((q < NR_PRIORITIES) && (q >= 0));
	list_del(&t->runq_link);
	
#ifdef _DEBUG_SCHED
	/* You can also just remove the specified process, but I just make sure it is
	 * really in our ready queue.
	 */
	LIST_FOR_EACH(l, &queue->threads[q]) {
		thrd = LIST_ENTRY(l, struct thread, runq_link);
		if (thrd == t) {
			found_times++;
		}
	}

	ASSERT(found_times == 0);		// The process should not be found
#endif	/* _DEBUG_SCHED */
}

static void sched_adjust_priority(struct sched_cpu *c, struct thread *t)
{
	;
}

static void sched_timer_func(void *ctx)
{
	CURR_THREAD->quantum = 0;
}

/**
 * Pick a new process from the queue to run
 */
static struct thread *sched_pick_thread(struct sched_cpu *c)
{
	struct thread *t;
	struct list *l;
	int q;

	t = NULL;
	
	/* Check each of the scheduling queues for ready processes. The number of
	 * queues is defined in process.h. The lowest queue contains IDLE, which
	 * is always ready.
	 */
	for (q = NR_PRIORITIES - 1; q >= 0; q--) {
		if (!LIST_EMPTY(&c->active->threads[q])) {
			l = c->active->threads[q].next;
			t = LIST_ENTRY(l, struct thread, runq_link);
			sched_dequeue(c->active, t);
			break;
		}
	}

	return t;
}

void sched_insert_thread(struct thread *t)
{
	sched_cpu_t *sched;

	ASSERT(t->state == THREAD_READY);
	
	t->cpu = sched_alloc_cpu(t);
	
	sched = t->cpu->sched;
	
	spinlock_acquire(&sched->lock);
	
	sched_enqueue(sched->active, t);
	sched->total++;

	spinlock_release(&sched->lock);

	DEBUG(DL_DBG, ("thread(%s:%d) inserted, total(%d).\n",
		       t->name, t->id, sched->total));
}

/**
 * Picks a new process to run and switches to it. Interrupts must be disable.
 * @param state		- Previous interrupt state
 */
void sched_reschedule(boolean_t state)
{
	struct sched_cpu *c;
	struct thread *next;

	/* Get current schedule CPU */
	c = CURR_CPU->sched;

	spinlock_acquire_noirq(&c->lock);

	/* Thread cannot be in ready state if we are running it now */
	ASSERT(CURR_THREAD->state != THREAD_READY);

	/* Adjust the priority of the thread based on whether it used up its quantum */
	if (CURR_THREAD != c->idle_thread) {
		sched_adjust_priority(c, CURR_THREAD);
	}

	/* Enqueue and dequeue the current process to update the process queue */
	if (CURR_THREAD->state == THREAD_RUNNING) {
		/* The thread hasn't gone to sleep, re-queue it */
		CURR_THREAD->state = THREAD_READY;
		if (CURR_THREAD != c->idle_thread) {
			sched_enqueue(c->active, CURR_THREAD);
		}
	} else {
		DEBUG(DL_DBG, ("thread(%s:%s:%d), state(%d).\n",
			       CURR_PROC->name, CURR_THREAD->name, CURR_THREAD->id,
			       CURR_THREAD->state));
		ASSERT(CURR_THREAD != c->idle_thread);
		c->total--;
		atomic_dec(&_nr_running_threads);
	}
	
	/* Find a new process to run. A NULL return value means no processes are
	 * ready, so we schedule the idle process in this case.
	 */
	next = sched_pick_thread(c);
	if (next) {
		next->quantum = THREAD_QUANTUM;
	} else {
		next = c->idle_thread;
		if (next != CURR_THREAD) {
			DEBUG(DL_DBG, ("cpu(%d) has no runnable threads.\n",
				       CURR_CPU->id));
		}
		next->quantum = 0;
	}

	/* Move the next process to running state and set it as the current */
	c->prev_thread = CURR_THREAD;
	next->state = THREAD_RUNNING;
	CURR_THREAD = next;

	/* Finished with the scheduler queues, release the lock */
	spinlock_release_noirq(&c->lock);

	/* Set off the timer if necessary */
	if (CURR_THREAD->quantum > 0) {
		set_timer(&c->timer, CURR_THREAD->quantum, sched_timer_func);
	}

	/* Perform the thread switch if current thread is not the same as previous
	 * one.
	 */
	if (CURR_THREAD != c->prev_thread) {
#ifdef _DEBUG_SCHED
		DEBUG(DL_DBG, ("switching to (%s:%d:%s:%d:%d).\n",
			       CURR_PROC->name, CURR_PROC->id, CURR_THREAD->name,
			       CURR_THREAD->id, CURR_CPU->id));
#endif	/* _DEBUG_SCHED */
		
		/* Switch the address space. The NULL case will be handled by the
		 * context switch function.
		 */
		mmu_switch_ctx(CURR_PROC->mmu_ctx);

		/* Perform the thread switch */
		arch_thread_switch(CURR_THREAD, c->prev_thread);

		/* Do all things need to do after swith */
		sched_post_switch(state);
	} else {
		spinlock_release_noirq(&CURR_THREAD->lock);
		irq_restore(state);
	}
}

void sched_post_switch(boolean_t state)
{
	struct thread *t;
	
	/* The prev_thread pointer is set to NULL during sched_init(). It will
	 * only be NULL once.
	 */
	t = CURR_CPU->sched->prev_thread;
	if (t) {

		/* Release the previous thread. We have performed switch if
		 * prev_thread is not NULL
		 */
		spinlock_release_noirq(&t->lock);
		
		/* Deal with thread terminations. We cannot delete the thread
		 * directly as all alloctor functions are unsafe to call here.
		 * Instead we queue the thread to the reaper's queue.
		 */
		if (t->state == THREAD_DEAD) {
			struct list *l;

			spinlock_acquire(&_dead_threads_lock);
			list_add_tail(&t->runq_link, &_dead_threads);
			spinlock_release(&_dead_threads_lock);
			
			DEBUG(DL_DBG, ("thread(%s:%d) -> dead threads list.\n", t->name, t->id));
			semaphore_up(&_dead_threads_sem, 1);
		}
	}

	irq_restore(state);
}

static void sched_reaper_thread(void *ctx)
{
	/* Reap the dead threads */
	struct list *p, *l;
	struct thread *t;

	/* If this is the first time reaper run, you should enable IRQ first */
	while (TRUE) {
		/* Wait for dead threads to be added to the list */
		semaphore_down(&_dead_threads_sem);

		spinlock_acquire(&_dead_threads_lock);

		ASSERT(!LIST_EMPTY(&_dead_threads));
		
		l = _dead_threads.next;
		list_del(l);

		spinlock_release(&_dead_threads_lock);

		t = LIST_ENTRY(l, struct thread, runq_link);
		
		DEBUG(DL_INF, ("release thread(%s:%d).\n", t->name, t->id));
		thread_release(t);
	}
}

static void sched_idle_thread(void *ctx)
{
	/* We run the loop with interrupts disabled. The cpu_idle() function
	 * is expected to re-enable interrupts as required.
	 */
	irq_disable();

	while (TRUE) {
		spinlock_acquire_noirq(&CURR_THREAD->lock);
		sched_reschedule(FALSE);
		
		cpu_idle();
	}
}

void init_sched_percpu()
{
	int i, j, rc = -1;
	char name[T_NAME_LEN];

	/* Initialize the scheduler for the current CPU */
	CURR_CPU->sched = kmalloc(sizeof(struct sched_cpu), 0);
	ASSERT(CURR_CPU->sched != NULL);

	spinlock_init(&CURR_CPU->sched->lock, "sched-lock");
	
	CURR_CPU->sched->total = 0;
	CURR_CPU->sched->active = &CURR_CPU->sched->queues[0];
	CURR_CPU->sched->expired = &CURR_CPU->sched->queues[1];

	/* Create the per CPU idle thread */
	snprintf(name, T_NAME_LEN - 1, "idle-%d", CURR_CPU->id);
	rc = thread_create(name, NULL, 0, sched_idle_thread, NULL,
			   &CURR_CPU->sched->idle_thread);
	ASSERT((rc == 0) && (CURR_CPU->sched->idle_thread != NULL));
	DEBUG(DL_DBG, ("idle thread(%p).\n",
		       CURR_CPU->sched->idle_thread));

	/* Set the idle thread as the current thread */
	CURR_CPU->sched->idle_thread->cpu = CURR_CPU;
	CURR_CPU->sched->idle_thread->state = THREAD_RUNNING;
	CURR_CPU->sched->prev_thread = NULL;
	CURR_CPU->thread = CURR_CPU->sched->idle_thread;
	
	/* Create the preemption timer */
	init_timer(&CURR_CPU->sched->timer, "sched-tmr", NULL);

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
	int rc = -1;

	semaphore_init(&_dead_threads_sem, "dead-t-sem", 0);

	spinlock_init(&_dead_threads_lock, "dead-t-lock");

	/* Create kernel mode reaper thread for the whole system */
	rc = thread_create("reaper", NULL, 0, sched_reaper_thread, NULL, NULL);
	ASSERT(rc == 0);

	DEBUG(DL_DBG, ("sched queues initialization done.\n"));
}

void sched_enter()
{
	/* Disable irq first */
	irq_disable();

	CURR_CPU->timer_enabled = TRUE;	// TODO: Find a better place to do this

	/* Switch to current process */
	arch_thread_switch(CURR_THREAD, NULL);
	PANIC("Should not get here");
}
