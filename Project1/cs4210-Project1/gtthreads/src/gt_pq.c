#include <stdio.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <assert.h>

#include "gt_include.h"


/**********************************************************************/
/* runqueue operations */
static void __add_to_runqueue(runqueue_t *runq, uthread_struct_t *u_elm);
static void __rem_from_runqueue(runqueue_t *runq, uthread_struct_t *u_elm);


/**********************************************************************/
/* runqueue operations */
static inline void __add_to_runqueue(runqueue_t *runq, uthread_struct_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->uthread_priority;
	ugroup = u_elem->uthread_gid;

	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_INSERT_TAIL(uhead, u_elem, uthread_runq);

	/* Update information */
	if(!IS_BIT_SET(runq->prio_array[uprio].group_mask, ugroup))
		SET_BIT(runq->prio_array[uprio].group_mask, ugroup);

	runq->uthread_tot++;

	runq->uthread_prio_tot[uprio]++;
	if(!IS_BIT_SET(runq->uthread_mask, uprio))
		SET_BIT(runq->uthread_mask, uprio);

	runq->uthread_group_tot[ugroup]++;
	if(!IS_BIT_SET(runq->uthread_group_mask[ugroup], uprio))
		SET_BIT(runq->uthread_group_mask[ugroup], uprio);

	return;
}

static inline void __rem_from_runqueue(runqueue_t *runq, uthread_struct_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->uthread_priority;
	ugroup = u_elem->uthread_gid;

	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_REMOVE(uhead, u_elem, uthread_runq);

	/* Update information */
	if(TAILQ_EMPTY(uhead))
		RESET_BIT(runq->prio_array[uprio].group_mask, ugroup);

	runq->uthread_tot--;

	if(!(--(runq->uthread_prio_tot[uprio])))
		RESET_BIT(runq->uthread_mask, uprio);

	if(!(--(runq->uthread_group_tot[ugroup])))
	{
		assert(TAILQ_EMPTY(uhead));
		RESET_BIT(runq->uthread_group_mask[ugroup], uprio);
	}

	return;
}

/* Returns credit for a group id (used when recharging) */
static inline unsigned int get_group_credit(unsigned int gid)
{
    return CREDIT_GROUPS[gid % NUM_CREDIT_GROUPS];
}

/* Move uthread to expires runqueue (assumes kthread_runqlock held by caller) */
static void migrate_to_expires(kthread_runqueue_t *kthread_runq, uthread_struct_t *u)
{
    runqueue_t *active = kthread_runq->active_runq;
    runqueue_t *expires = kthread_runq->expires_runq;

    /* remove from active */
    __rem_from_runqueue(active, u);
    /* insert into expires - lock already held by caller */
    __add_to_runqueue(expires, u);
}

/**********************************************************************/
/* Exported runqueue operations */
extern void init_runqueue(runqueue_t *runq)
{
	uthread_head_t *uhead;
	int i, j;
	/* Everything else is global, so already initialized to 0(correct init value) */
	for(i=0; i<MAX_UTHREAD_PRIORITY; i++)
	{
		for(j=0; j<MAX_UTHREAD_GROUPS; j++)
		{
			uhead = &((runq)->prio_array[i].group[j]);
			TAILQ_INIT(uhead);
		}
	}
	return;
}

extern void add_to_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem)
{
	gt_spin_lock(runq_lock);
	runq_lock->holder = 0x02;
	__add_to_runqueue(runq, u_elem);
	gt_spin_unlock(runq_lock);
	return;
}

extern void rem_from_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock, uthread_struct_t *u_elem)
{
	gt_spin_lock(runq_lock);
	runq_lock->holder = 0x03;
	__rem_from_runqueue(runq, u_elem);
	gt_spin_unlock(runq_lock);
	return;
}

extern void switch_runqueue(runqueue_t *from_runq, gt_spinlock_t *from_runqlock, 
		runqueue_t *to_runq, gt_spinlock_t *to_runqlock, uthread_struct_t *u_elem)
{
	rem_from_runqueue(from_runq, from_runqlock, u_elem);
	add_to_runqueue(to_runq, to_runqlock, u_elem);
	return;
}

/* Initialize single uthread's credits (call on uthread create or first add) */
extern void init_uthread_credits(uthread_struct_t *u)
{
    unsigned int gid = u->uthread_gid; // get group_id
    unsigned int idx = gid;
    u->uthread_credits = CREDIT_GROUPS[idx]; // get credits available
	u->initial_credits = u->uthread_credits;
}

/* Recharge credits for all uthreads in a runqueue (walks all priorities/groups) */
extern void recharge_runqueue_credits(runqueue_t *runq)
{
    int p,g;
    uthread_struct_t *u;
    uthread_head_t *head;
    for(p = 0; p < MAX_UTHREAD_PRIORITY; p++) { // iterate through each priority queue
        prio_struct_t *ps = &runq->prio_array[p];
        for(g = 0; g < MAX_UTHREAD_GROUPS; g++) {
            head = &ps->group[g];
            TAILQ_FOREACH(u, head, uthread_runq) {
                /* Refill to group default (simple policy). */
                u->uthread_credits = get_group_credit(g);
            }
        }
    }
}



/**********************************************************************/

extern void kthread_init_runqueue(kthread_runqueue_t *kthread_runq)
{
	kthread_runq->active_runq = &(kthread_runq->runqueues[0]);
	kthread_runq->expires_runq = &(kthread_runq->runqueues[1]);

	gt_spinlock_init(&(kthread_runq->kthread_runqlock));
	init_runqueue(kthread_runq->active_runq);
	init_runqueue(kthread_runq->expires_runq);

	TAILQ_INIT(&(kthread_runq->zombie_uthreads));
	return;
}

extern void print_runq_stats(runqueue_t *runq, char *runq_str)
{
	int inx;
	printf("\n******************************************************\n");
	unsigned int cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;
	printf("Run queue(%s) state (CPU #%d) : \n", runq_str, cpuid);
	printf("******************************************************\n");
	printf("uthreads details - (tot:%d , mask:%x)\n", runq->uthread_tot, runq->uthread_mask);
	// printf("******************************************************\n");
	// printf("uthread priority details : \n");
	// for(inx=0; inx<MAX_UTHREAD_PRIORITY; inx++)
	// 	printf("uthread priority (%d) - (tot:%d)\n", inx, runq->uthread_prio_tot[inx]);
	// printf("******************************************************\n");
	// printf("uthread group details : \n");
	// for(inx=0; inx<MAX_UTHREAD_GROUPS; inx++)
	// 	printf("uthread group (%d) - (tot:%d , mask:%x)\n", inx, runq->uthread_group_tot[inx], runq->uthread_group_mask[inx]);
	printf("******************************************************\n");

	/* ADD THIS SECTION FOR CREDIT SCHEDULER */
    if (scheduler_type == SCHED_CREDIT && runq->uthread_tot > 0) {
        printf("uthread credits : ");
        for(int p = 0; p < MAX_UTHREAD_PRIORITY; p++) {
            if (!IS_BIT_SET(runq->uthread_mask, p)) continue;
            for(int g = 0; g < MAX_UTHREAD_GROUPS; g++) {
                if (!IS_BIT_SET(runq->prio_array[p].group_mask, g)) continue;
                
                uthread_head_t *head = &runq->prio_array[p].group[g];
                uthread_struct_t *u;
                TAILQ_FOREACH(u, head, uthread_runq) {
                    printf("T%d(%dc) ", u->uthread_tid, u->uthread_credits);
                }
            }
        }
        printf("\n******************************************************\n");
    }

	return;
}

/* Change to make function choice (dispatch to credit or priority) */ 
extern uthread_struct_t *sched_find_best_uthread(kthread_runqueue_t *kthread_runq)
{
	uthread_struct_t *u_obj;

	// If statement to branch for credit or priority scheduler
	if (scheduler_type == SCHED_CREDIT) 
    	u_obj = sched_find_best_uthread_credit(kthread_runq);
	else
    	u_obj = sched_find_best_uthread_priority(kthread_runq);

	// If statement to include load balancing if enabled
	if (!u_obj && load_balancing_enabled) {
        u_obj = load_balance(kthread_runq);

		// if (u_obj) {
        //     printf("[LOAD_BALANCE] Successfully stole work, continuing execution\n");
        // }
	}

	return u_obj;
}

extern uthread_struct_t *sched_find_best_uthread_priority(kthread_runqueue_t *kthread_runq) {
	/* [1] Tries to find the highest priority RUNNABLE uthread in active-runq.
	 * [2] Found - Jump to [FOUND]
	 * [3] Switches runqueues (active/expires)
	 * [4] Repeat [1] through [2]
	 * [NOT FOUND] Return NULL(no more jobs)
	 * [FOUND] Remove uthread from pq and return it. */

	runqueue_t *runq;
	prio_struct_t *prioq; // group of runq
	uthread_head_t *u_head; // head of runq
	uthread_struct_t *u_obj; // current thread and its info
	unsigned int uprio, ugroup;

	gt_spin_lock(&(kthread_runq->kthread_runqlock));

	runq = kthread_runq->active_runq;

	kthread_runq->kthread_runqlock.holder = 0x04;
	if(!(runq->uthread_mask))
	{ /* No jobs in active. switch runqueue */
		assert(!runq->uthread_tot);
		kthread_runq->active_runq = kthread_runq->expires_runq;
		kthread_runq->expires_runq = runq;

		runq = kthread_runq->expires_runq;
		if(!runq->uthread_mask)
		{
			assert(!runq->uthread_tot);
			gt_spin_unlock(&(kthread_runq->kthread_runqlock));
			return NULL;
		}
	}

	/* Find the highest priority bucket */
	uprio = LOWEST_BIT_SET(runq->uthread_mask);
	prioq = &(runq->prio_array[uprio]);

	assert(prioq->group_mask);
	ugroup = LOWEST_BIT_SET(prioq->group_mask);

	u_head = &(prioq->group[ugroup]);
	u_obj = TAILQ_FIRST(u_head);
	__rem_from_runqueue(runq, u_obj);

	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
#if 0
	printf("cpu(%d) : sched best uthread(id:%d, group:%d)\n", u_obj->cpu_id, u_obj->uthread_tid, u_obj->uthread_gid);
#endif
	return(u_obj);
}

extern uthread_struct_t *sched_find_best_uthread_credit(kthread_runqueue_t *kthread_runq)
{
	runqueue_t *runq;
    prio_struct_t *prioq;
    uthread_head_t *u_head;
    uthread_struct_t *u_obj;
	uthread_struct_t *global_best_thread = NULL;
    unsigned int uprio, ugroup;
	int global_max_credits = 0;

    gt_spin_lock(&(kthread_runq->kthread_runqlock));
    kthread_runq->kthread_runqlock.holder = 0x04;
	
    runq = kthread_runq->active_runq;

    // If active empty -> swap and recharge
    if (!(runq->uthread_mask)) {
        assert(!runq->uthread_tot);
        kthread_runq->active_runq = kthread_runq->expires_runq;
        kthread_runq->expires_runq = runq;


        runq = kthread_runq->expires_runq;
        if (!runq->uthread_mask) {
            assert(!runq->uthread_tot);
            gt_spin_unlock(&(kthread_runq->kthread_runqlock));
            return NULL;
        }
        recharge_runqueue_credits(kthread_runq->active_runq); // recharge all creds
		// print_runq_stats(kthread_runq->active_runq, "ACTIVE");
    }

    // Scan ALL priority levels and groups to find absolute MAX credits
	for (int p = 0; p < MAX_UTHREAD_PRIORITY; p++) {
		if (!IS_BIT_SET(runq->uthread_mask, p)) continue;
		
		prio_struct_t *prioq = &(runq->prio_array[p]);
		for (int g = 0; g < MAX_UTHREAD_GROUPS; g++) {
			if (!IS_BIT_SET(prioq->group_mask, g)) continue;
			
			uthread_head_t *u_head = &(prioq->group[g]);
			uthread_struct_t *u_obj;
			
			// printf("\n[sched] scanning priority=%d group=%d\n", p, g);
			
			TAILQ_FOREACH(u_obj, u_head, uthread_runq) {
				// printf("[sched] checking tid=%d credits=%d (current_best=%d)\n", 
					// u_obj->uthread_tid, u_obj->uthread_credits, global_max_credits);
				
				if (u_obj->uthread_credits > global_max_credits) {
					global_max_credits = u_obj->uthread_credits;
					global_best_thread = u_obj;
					// printf("[sched] NEW BEST: tid=%d credits=%d\n", 
					// 	u_obj->uthread_tid, u_obj->uthread_credits);
				}
			}
		}
	}

	if (global_best_thread && global_best_thread->uthread_credits > 0) {
		// Found thread with highest credtis globalyl - remove and return
		// print_runq_stats(runq, "ACTIVE");
		// print_runq_stats(kthread_runq->expires_runq, "EXPIRED");
		__rem_from_runqueue(runq, global_best_thread);
		printf("\n[sched] picked GLOBAL HIGHEST CREDITS tid=%d gid=%d -> ACTIVE (credits=%d)\n",
				global_best_thread->uthread_tid, global_best_thread->uthread_gid, global_best_thread->uthread_credits);
		gt_spin_unlock(&(kthread_runq->kthread_runqlock));
		
		return global_best_thread;
	}

	/* If no threads have credits > 0, move all to expires queue */
	// printf("\n[sched] No threads with credits > 0, migrating all to expires\n");
	for (int p = 0; p < MAX_UTHREAD_PRIORITY; p++) {
		if (!IS_BIT_SET(runq->uthread_mask, p)) continue;
		
		prio_struct_t *prioq = &(runq->prio_array[p]);
		for (int g = 0; g < MAX_UTHREAD_GROUPS; g++) {
			if (!IS_BIT_SET(prioq->group_mask, g)) continue;
			
			uthread_head_t *u_head = &(prioq->group[g]);
			uthread_struct_t *u_obj = TAILQ_FIRST(u_head);
			
			while (u_obj) {
				uthread_struct_t *next = TAILQ_NEXT(u_obj, uthread_runq);
				__rem_from_runqueue(runq, u_obj);
				__add_to_runqueue(kthread_runq->expires_runq, u_obj);
				// printf("\n[sched] migrating tid=%d gid=%d -> EXPIRES (credits=%d)\n",
				// 		u_obj->uthread_tid, u_obj->uthread_gid, u_obj->uthread_credits);
				u_obj = next;
			}
		}
	}
    /* If we reach here, no runnable with credits in active: swap and recharge */
    kthread_runq->active_runq = kthread_runq->expires_runq;
    kthread_runq->expires_runq = runq;
    runq = kthread_runq->active_runq;
    if (!runq->uthread_mask) {
        assert(!runq->uthread_tot);
        gt_spin_unlock(&(kthread_runq->kthread_runqlock));
        return NULL;
    }
    recharge_runqueue_credits(runq);

    /* For simplicity call function again (we already hold/released lock above, so re-lock) */
    gt_spin_unlock(&(kthread_runq->kthread_runqlock));
    return sched_find_best_uthread_credit(kthread_runq);
}



/* XXX: More work to be done !!! */
extern gt_spinlock_t uthread_group_penalty_lock;
extern unsigned int uthread_group_penalty;

extern uthread_struct_t *sched_find_best_uthread_group(kthread_runqueue_t *kthread_runq)
{
	/* [1] Tries to find a RUNNABLE uthread in active-runq from u_gid.
	 * [2] Found - Jump to [FOUND]
	 * [3] Tries to find a thread from a group with least threads in runq (XXX: NOT DONE)
	 * - [Tries to find the highest priority RUNNABLE thread (XXX: DONE)]
	 * [4] Found - Jump to [FOUND]
	 * [5] Switches runqueues (active/expires)
	 * [6] Repeat [1] through [4]
	 * [NOT FOUND] Return NULL(no more jobs)
	 * [FOUND] Remove uthread from pq and return it. */
	runqueue_t *runq;
	prio_struct_t *prioq;
	uthread_head_t *u_head;
	uthread_struct_t *u_obj;
	unsigned int uprio, ugroup, mask;
	uthread_group_t u_gid;

#ifndef COSCHED
	return sched_find_best_uthread(kthread_runq);
#endif

	/* XXX: Read u_gid from global uthread-select-criterion */
	u_gid = 0;
	runq = kthread_runq->active_runq;

	if(!runq->uthread_mask)
	{ /* No jobs in active. switch runqueue */
		assert(!runq->uthread_tot);
		kthread_runq->active_runq = kthread_runq->expires_runq;
		kthread_runq->expires_runq = runq;

		runq = kthread_runq->expires_runq;
		if(!runq->uthread_mask)
		{
			assert(!runq->uthread_tot);
			return NULL;
		}
	}


	if(!(mask = runq->uthread_group_mask[u_gid]))
	{ /* No uthreads in the desired group */
		assert(!runq->uthread_group_tot[u_gid]);
		return (sched_find_best_uthread(kthread_runq));
	}

	/* Find the highest priority bucket for u_gid */
	uprio = LOWEST_BIT_SET(mask);

	/* Take out a uthread from the bucket. Return it. */
	u_head = &(runq->prio_array[uprio].group[u_gid]);
	u_obj = TAILQ_FIRST(u_head);
	rem_from_runqueue(runq, &(kthread_runq->kthread_runqlock), u_obj);

	return(u_obj);
}

extern uthread_struct_t *load_balance(kthread_runqueue_t *idle_runq) {
	kthread_context_t *idle_k_ctx, *victim_k_ctx, *best_victim = NULL;
    kthread_runqueue_t *victim_runq, *best_victim_runq = NULL;
    uthread_struct_t *stolen_uthread = NULL;
    int max_work = 0, victim_work;
    int idle_cpu_id, victim_cpu_id;

	if (!load_balancing_enabled)
		return NULL;

	idle_k_ctx = kthread_cpu_map[kthread_apic_id()];
    idle_cpu_id = idle_k_ctx->cpuid;

	// Selecting a victim CPU (CPU with highest amount of uthreads)
	for (int cpu_idx = 0; cpu_idx < GT_MAX_KTHREADS; cpu_idx++)
	{
		victim_k_ctx = kthread_cpu_map[cpu_idx];

		// skip if our own kthread, or kthread not assigned
		if (!victim_k_ctx || victim_k_ctx == idle_k_ctx)
            continue;

		// Skip kthreads markde as done (finished execution)
        if (victim_k_ctx->kthread_flags & KTHREAD_DONE)
            continue;

		victim_runq = &(victim_k_ctx->krunqueue);
		victim_cpu_id = victim_k_ctx->cpuid;

		gt_spin_lock(&(victim_runq->kthread_runqlock));
		victim_work = victim_runq->active_runq->uthread_tot + victim_runq->expires_runq->uthread_tot;
		gt_spin_unlock(&(victim_runq->kthread_runqlock));

		if (max_work < victim_work) {
			best_victim_runq = victim_runq;
			best_victim = victim_k_ctx;
			max_work = victim_work;
		}

	}

	// Could not find any other cpu with work (entire system is idle)
	if (!best_victim || max_work <= 1)
		return NULL;

	printf("[LOAD_BALANCE] CPU %d selected CPU %d as victim (total_work=%d)\n", 
           idle_cpu_id, best_victim->cpuid, max_work);

	gt_spin_lock(&(best_victim_runq->kthread_runqlock));

	runqueue_t *source_runq = best_victim_runq->active_runq; // getting runqueue of victim
    if (!source_runq->uthread_tot) { // switch to expired if active runqueue has no uthreads in it
        source_runq = best_victim_runq->expires_runq;
        // printf("[LOAD_BALANCE] Active queue empty, stealing from expires queue\n");
    }

	if (source_runq->uthread_tot > 0) {
		// bit mask ops to acquire the heda of the list of uthreads
		unsigned int uprio = LOWEST_BIT_SET(source_runq->uthread_mask);
        prio_struct_t *prioq = &(source_runq->prio_array[uprio]);
        unsigned int ugroup = LOWEST_BIT_SET(prioq->group_mask);
        uthread_head_t *u_head = &(prioq->group[ugroup]);
        
        // Get the first uthread from the selected priority/group queue
        stolen_uthread = TAILQ_FIRST(u_head);
        if (stolen_uthread) {
			printf("\n[LOAD_BALANCE] ===== QUEUE STATUS BEFORE LOAD BALANCING =====\n");
			printf("[LOAD_BALANCE] Victim CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
				best_victim->cpuid,
				best_victim_runq->active_runq->uthread_tot,
				best_victim_runq->expires_runq->uthread_tot);
			print_runq_stats(best_victim_runq->active_runq, "ACTIVE_VICTIM");
			// printf("[LOAD_BALANCE] Previously Idle CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
			// 	idle_cpu_id,
			// 	idle_runq->active_runq->uthread_tot,
			// 	idle_runq->expires_runq->uthread_tot);
            // Remove uthread from victim runqueue
            __rem_from_runqueue(source_runq, stolen_uthread);
            
            // Update the stolen uthread CPU assignment
            stolen_uthread->cpu_id = idle_cpu_id;
            stolen_uthread->last_cpu_id = idle_cpu_id;
            
            printf("\n[LOAD_BALANCE] Successfully stole uthread %d (group %d) from CPU %d to CPU %d\n",
                   stolen_uthread->uthread_tid, stolen_uthread->uthread_gid,
                   best_victim->cpuid, idle_cpu_id);
			
			printf("\n[LOAD_BALANCE] ===== QUEUE STATUS AFTER LOAD BALANCING =====\n");
			printf("[LOAD_BALANCE] Victim CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
				best_victim->cpuid,
				best_victim_runq->active_runq->uthread_tot,
				best_victim_runq->expires_runq->uthread_tot);
			print_runq_stats(best_victim_runq->active_runq, "ACTIVE_VICTIM");
			// printf("[LOAD_BALANCE] Previously Idle CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
			// 	idle_cpu_id,
			// 	idle_runq->active_runq->uthread_tot,
			// 	idle_runq->expires_runq->uthread_tot);
        }
	}
	gt_spin_unlock(&(best_victim_runq->kthread_runqlock));

	// if (stolen_uthread) {
	// 	printf("\n[LOAD_BALANCE] ===== QUEUE STATUS AFTER LOAD BALANCING =====\n");

	// 	// Print victim's updated queue status
	// 	gt_spin_lock(&(best_victim_runq->kthread_runqlock));
	// 	printf("[LOAD_BALANCE] Victim CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
	// 		best_victim->cpuid,
	// 		best_victim_runq->active_runq->uthread_tot,
	// 		best_victim_runq->expires_runq->uthread_tot);
	// 	gt_spin_unlock(&(best_victim_runq->kthread_runqlock));

	// 	// Print idle (now working) kthread's queue status  
	// 	gt_spin_lock(&(idle_runq->kthread_runqlock));
	// 	printf("[LOAD_BALANCE] Previously Idle CPU %d - Active: %d uthreads, Expires: %d uthreads\n",
	// 		idle_cpu_id,
	// 		idle_runq->active_runq->uthread_tot,
	// 		idle_runq->expires_runq->uthread_tot);
	// 	gt_spin_unlock(&(idle_runq->kthread_runqlock));

	// 	printf("[LOAD_BALANCE] ================================================\n\n");
	// }

	return stolen_uthread;
	
}

#if 0
/*****************************************************************************************/
/* Main Test Function */

runqueue_t active_runqueue, expires_runqueue;

#define MAX_UTHREADS 1000
uthread_struct_t u_objs[MAX_UTHREADS];

static void fill_runq(runqueue_t *runq)
{
	uthread_struct_t *u_obj;
	int inx;
	/* create and insert */
	for(inx=0; inx<MAX_UTHREADS; inx++)
	{
		u_obj = &u_objs[inx];
		u_obj->uthread_tid = inx;
		u_obj->uthread_gid = (inx % MAX_UTHREAD_GROUPS);
		u_obj->uthread_priority = (inx % MAX_UTHREAD_PRIORITY);
		__add_to_runqueue(runq, u_obj);
		printf("Uthread (id:%d , prio:%d) inserted\n", u_obj->uthread_tid, u_obj->uthread_priority);
	}

	return;
}

static void change_runq(runqueue_t *from_runq, runqueue_t *to_runq)
{
	uthread_struct_t *u_obj;
	int inx;
	/* Remove and delete */
	for(inx=0; inx<MAX_UTHREADS; inx++)
	{
		u_obj = &u_objs[inx];
		switch_runqueue(from_runq, to_runq, u_obj);
		printf("Uthread (id:%d , prio:%d) moved\n", u_obj->uthread_tid, u_obj->uthread_priority);
	}

	return;
}


static void empty_runq(runqueue_t *runq)
{
	uthread_struct_t *u_obj;
	int inx;
	/* Remove and delete */
	for(inx=0; inx<MAX_UTHREADS; inx++)
	{
		u_obj = &u_objs[inx];
		__rem_from_runqueue(runq, u_obj);
		printf("Uthread (id:%d , prio:%d) removed\n", u_obj->uthread_tid, u_obj->uthread_priority);
	}

	return;
}

int main()
{
	runqueue_t *active_runq, *expires_runq;
	uthread_struct_t *u_obj;
	int inx;

	active_runq = &active_runqueue;
	expires_runq = &expires_runqueue;

	init_runqueue(active_runq);
	init_runqueue(expires_runq);

	fill_runq(active_runq);
	print_runq_stats(active_runq, "ACTIVE");
	print_runq_stats(expires_runq, "EXPIRES");
	change_runq(active_runq, expires_runq);
	print_runq_stats(active_runq, "ACTIVE");
	print_runq_stats(expires_runq, "EXPIRES");
	empty_runq(expires_runq);
	print_runq_stats(active_runq, "ACTIVE");
	print_runq_stats(expires_runq, "EXPIRES");

	return 0;
}

#endif
