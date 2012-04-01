#include <types.h>
#include <stddef.h>
#include "task.h"
#include "sched.h"

/* The schedule queue for our kernel */
struct task *_ready_head[NR_SCHED_QUEUES];
struct task *_ready_tail[NR_SCHED_QUEUES];

/* Pointer to our various task */
struct task *_next_task = NULL;
struct task *_prev_task = NULL;
struct task *_curr_task = NULL;				// Current running task

static void sched(struct task *tp, int *queue, boolean_t *front);

static void pick_task()
{
	/* Decide who to run now. A new task is selected by setting `_next_task'.
	 */
	struct task *tp;
	int i;

	/* Check each of the scheduling queues for ready tasks. The number of
	 * queues is defined in task.h. The lowest queue contains IDLE, which
	 * is always ready.
	 */
	for (i = 0; i < NR_SCHED_QUEUES; i++) {
		if ((tp = _ready_head[i]) != NULL) {
			_next_task = tp;
			return;
		}
	}
}

void sched_init()
{
	memset(&_ready_head[0], 0, sizeof(struct task *) * NR_SCHED_QUEUES);
	memset(&_ready_tail[0], 0, sizeof(struct task *) * NR_SCHED_QUEUES);
}

/**
 * Add `tp' to one of the queues of runnable tasks. This function is responsible
 * for inserting a task into one of the scheduling queues.
 */
void sched_enqueue(struct task *tp)
{
	int i;
	boolean_t front;

	/* Determine where to insert the task */
	sched(tp, &i, &front);
	
	/* Now add the task to the queue. */
	if (_ready_head[i] == NULL) {			// Add to empty queue
		_ready_head[i] = _ready_tail[i] = tp;
		tp->next = NULL;
	} else if (front) {				// Add to head of queue
		tp->next = _ready_head[i];
		_ready_head[i] = tp;
	} else {					// Add to tail of queue
		_ready_tail[i]->next = tp;
		_ready_tail[i] = tp;
		tp->next = NULL;
	}

	/* Now select the next task to run */
	pick_task();
}

/**
 * A task must be removed from the scheduling queues, for example, because it has
 * been blocked. If current active task is removed, a new task is picked by calling
 * pick_task()
 */
void sched_dequeue(struct task *tp)
{
	int i;
	struct task **xpp;
	struct task *prev_ptr;

	i = tp->priority;

	/* Now make sure that the task is not in its ready queue. Remove the task
	 * if it was found.
	 */
	prev_ptr = NULL;
	for (xpp = &_ready_head[i]; *xpp != NULL; xpp = &((*xpp)->next)) {
		
		if (*xpp == tp) {			// Found task to remove
			*xpp = (*xpp)->next;		// Replace with the next in chain
			if (tp == _ready_tail[i])	// Queue tail removed
				_ready_tail[i] = prev_ptr; // Set new tail
			if ((tp == _curr_task) || (tp == _next_task)) // Active task removed
				pick_task();		// Pick a new task to run
			break;				// Break out the for loop
		}
		prev_ptr = *xpp;
	}
}

/**
 * This function determines the scheduling policy. It is called whenever a task
 * must be added to one of the scheduling queues to decide where to insert it.
 * As a side-effect the process' priority may be updated.
 */
static void sched(struct task *tp, int *queue, boolean_t *front)
{
	boolean_t time_left = (tp->ticks_left > 0);	// Quantum fully consumed
	int penalty = 0;				// Change in priority

	/* Check whether the task has time left. Otherwise give a new quantum
	 * and possibly raise the priority.
	 */
	if (!time_left) {				// Quantum consumed ?
		tp->ticks_left = tp->quantum;		// Give new quantum
		if (_prev_task == tp)
			penalty++;			// Priority boost
		else
			penalty--;			// Give slow way back
		_prev_task = tp;			// Store ptr for next
	}

	/* Determine the new priority of this task. */
	if (penalty != 0) {
		tp->priority += penalty;		// Update with penalty
		if (tp->priority < tp->max_priority)	// Check upper bound
			tp->priority = tp->max_priority;
		else if (tp->priority > (IDLE_Q + 1))	// Check lower bound
			tp->priority = IDLE_Q;
	}

	/* If there is time left, the task is added to the front of its queue,
	 * so that it can run immediately. The queue to use is simply the current
	 * task's priority.
	 */
	*queue = tp->priority;
	*front = time_left;
}
