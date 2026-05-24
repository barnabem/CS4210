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
/** DECLARATIONS **/
/**********************************************************************/


/**********************************************************************/
/* kthread runqueue and env */

/* XXX: should be the apic-id */
#define KTHREAD_CUR_ID	0

/**********************************************************************/
/* uthread scheduling */
static void uthread_context_func(int);
static int uthread_init(uthread_struct_t *u_new);

/**********************************************************************/
/* uthread creation */
#define UTHREAD_DEFAULT_SSIZE (16 * 1024)

extern int uthread_create(uthread_t *u_tid, int (*u_func)(void *), void *u_arg, uthread_group_t u_gid);

/**********************************************************************/
/** DEFNITIONS **/
/**********************************************************************/



/**********************************************************************/
/* uthread scheduling */

/* Assumes that the caller has disabled vtalrm and sigusr1 signals */
/* uthread_init will be using */
static int uthread_init(uthread_struct_t *u_new)
{
	stack_t oldstack;
	sigset_t set, oldset;
	struct sigaction act, oldact;

	gt_spin_lock(&(ksched_shared_info.uthread_init_lock));

	/* Register a signal(SIGUSR2) for alternate stack */
	act.sa_handler = uthread_context_func;
	act.sa_flags = (SA_ONSTACK | SA_RESTART);
	if(sigaction(SIGUSR2,&act,&oldact))
	{
		fprintf(stderr, "uthread sigusr2 install failed !!");
		return -1;
	}

	/* Install alternate signal stack (for SIGUSR2) */
	if(sigaltstack(&(u_new->uthread_stack), &oldstack))
	{
		fprintf(stderr, "uthread sigaltstack install failed.");
		return -1;
	}

	/* Unblock the signal(SIGUSR2) */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_UNBLOCK, &set, &oldset);


	/* SIGUSR2 handler expects kthread_runq->cur_uthread
	 * to point to the newly created thread. We will temporarily
	 * change cur_uthread, before entering the synchronous call
	 * to SIGUSR2. */

	/* kthread_runq is made to point to this new thread
	 * in the caller. Raise the signal(SIGUSR2) synchronously */
#if 0
	raise(SIGUSR2);
#endif
	syscall(__NR_tkill, kthread_cpu_map[kthread_apic_id()]->tid, SIGUSR2);

	/* Block the signal(SIGUSR2) */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	if(sigaction(SIGUSR2,&oldact,NULL))
	{
		fprintf(stderr, "uthread sigusr2 revert failed !!");
		return -1;
	}

	/* Disable the stack for signal(SIGUSR2) handling */
	u_new->uthread_stack.ss_flags = SS_DISABLE;

	/* Restore the old stack/signal handling */
	if(sigaltstack(&oldstack, NULL))
	{
		fprintf(stderr, "uthread sigaltstack revert failed.");
		return -1;
	}

	gt_spin_unlock(&(ksched_shared_info.uthread_init_lock));
	return 0;
}

// Add elapsed time to the accumulator
static void accumulate_cpu_time(uthread_struct_t *thread)
{
    struct timeval current_time, elapsed;
    gettimeofday(&current_time, NULL);
    
    elapsed.tv_sec = current_time.tv_sec - thread->start_time.tv_sec;
    elapsed.tv_usec = current_time.tv_usec - thread->start_time.tv_usec;

    if (elapsed.tv_usec < 0) {
        elapsed.tv_sec--;
        elapsed.tv_usec += 1000000;
    }
    
    thread->total_cpu_time.tv_sec += elapsed.tv_sec;
    thread->total_cpu_time.tv_usec += elapsed.tv_usec;
    
    if (thread->total_cpu_time.tv_usec >= 1000000) {
        thread->total_cpu_time.tv_sec++;
        thread->total_cpu_time.tv_usec -= 1000000;
    }
}

static void timeval_addition(struct timeval *result_tv, struct timeval *tv1, struct timeval *tv2)
{
    result_tv->tv_sec = tv1->tv_sec + tv2->tv_sec;
    result_tv->tv_usec = tv1->tv_usec + tv2->tv_usec;
    
    if (result_tv->tv_usec >= 1000000) {
        result_tv->tv_sec++;
        result_tv->tv_usec -= 1000000;
    }
}


static void timeval_subtraction(struct timeval *result_tv, struct timeval *tv1, struct timeval *tv2)
{
    result_tv->tv_sec = tv1->tv_sec - tv2->tv_sec;
    result_tv->tv_usec = tv1->tv_usec - tv2->tv_usec;

    if (result_tv->tv_usec < 0) {
        result_tv->tv_sec--;
        result_tv->tv_usec += 1000000;
    }
}

static void init_cpu_time_tracking(uthread_struct_t *thread)
{
    thread->total_cpu_time.tv_sec = 0;
    thread->total_cpu_time.tv_usec = 0;
	thread->total_time.tv_sec = 0;
    thread->total_time.tv_usec = 0;
	gettimeofday(&(thread->first_start_time), NULL);
}

static long timeval_to_us(struct timeval *tv) 
{
    return tv->tv_sec * 1000000 + tv->tv_usec;
}

extern void uthread_schedule(uthread_struct_t * (*kthread_best_sched_uthread)(kthread_runqueue_t *))
{
	kthread_context_t *k_ctx;
	kthread_runqueue_t *kthread_runq;
	uthread_struct_t *u_obj;

	/* Signals used for cpu_thread scheduling */
	// kthread_block_signal(SIGVTALRM);
	// kthread_block_signal(SIGUSR1);

#if 0
	fprintf(stderr, "uthread_schedule invoked !!\n");
#endif

	k_ctx = kthread_cpu_map[kthread_apic_id()];
	kthread_runq = &(k_ctx->krunqueue);

	if((u_obj = kthread_runq->cur_uthread))
	{
		
		accumulate_cpu_time(u_obj);

		/*Go through the runq and schedule the next thread to run */
		kthread_runq->cur_uthread = NULL;

		if(u_obj->uthread_state & (UTHREAD_DONE | UTHREAD_CANCELLED))
		{
			struct timeval end_time;
			gettimeofday(&end_time, NULL);
			timeval_subtraction(&(u_obj->total_time), &end_time, &(u_obj->first_start_time));
			
			if (LOG) {
				long total_time_us = timeval_to_us(&(u_obj->total_time));
				long cpu_time_us = timeval_to_us(&(u_obj->total_cpu_time));
    			long wait_time_us = total_time_us - cpu_time_us;
				fprintf(stderr, "%lu,%lu,%lu\n", cpu_time_us, wait_time_us, total_time_us);
			} else {
				fprintf(stderr, "credits: %d) finished (TOTAL TIME: %lu s and %lu us; CPU TIME: %lu s and %lu us)",
    		        u_obj->initial_credits,
    		        u_obj->total_time.tv_sec, u_obj->total_time.tv_usec,
   	 		        u_obj->total_cpu_time.tv_sec, u_obj->total_cpu_time.tv_usec);
			}
			


			/* XXX: Inserting uthread into zombie queue is causing improper
			 * cleanup/exit of uthread (core dump) */
			uthread_head_t * kthread_zhead = &(kthread_runq->zombie_uthreads);
			gt_spin_lock(&(kthread_runq->kthread_runqlock));
			kthread_runq->kthread_runqlock.holder = 0x01;
			TAILQ_INSERT_TAIL(kthread_zhead, u_obj, uthread_runq);
			gt_spin_unlock(&(kthread_runq->kthread_runqlock));

			{
				ksched_shared_info_t *ksched_info = &ksched_shared_info;	
				gt_spin_lock(&ksched_info->ksched_lock);
				ksched_info->kthread_cur_uthreads--;
				gt_spin_unlock(&ksched_info->ksched_lock);
			}
		}
		else
		{
            u_obj->uthread_state = UTHREAD_RUNNABLE;

            if (scheduler_type == SCHED_CREDIT) {
                // calculate actual runtime and deduct credits accordignly 
                struct timeval end_time, diff;
                gettimeofday(&end_time, NULL);
               
                long runtime_us = (end_time.tv_sec - u_obj->start_time.tv_sec) * 1000000 +
                                (end_time.tv_usec - u_obj->start_time.tv_usec);
               
                int runtime_ms = runtime_us / 1000;
				
                // deduct credits based on actual runtime (1 credit per millisecond)
                if (runtime_ms > 0) {
					int old_creds = u_obj->uthread_credits;
                    u_obj->uthread_credits -= runtime_ms;
                    if (u_obj->uthread_credits < 0)
                        u_obj->uthread_credits = 0;
					// printf("\n[sched] deducted credits tid=%d gid=%d, old_creds=%d, new_creds=%d\n",
       				// 	u_obj->uthread_tid, u_obj->uthread_gid, old_creds, u_obj->uthread_credits);
                       
                }
			}

			add_to_runqueue(kthread_runq->active_runq, &(kthread_runq->kthread_runqlock), u_obj);
			/* XXX: Save the context (signal mask not saved) */
			if(sigsetjmp(u_obj->uthread_env, 0))
				return;
		}
	}

	/* kthread_best_sched_uthread acquires kthread_runqlock. Dont lock it up when calling the function. */
	if(!(u_obj = kthread_best_sched_uthread(kthread_runq)))
	{
		/* Done executing all uthreads. Return to main */
		/* XXX: We can actually get rid of KTHREAD_DONE flag */
		if(ksched_shared_info.kthread_tot_uthreads && !ksched_shared_info.kthread_cur_uthreads)
		{
			fprintf(stderr, "Quitting kthread (%d)\n", k_ctx->cpuid);
			k_ctx->kthread_flags |= KTHREAD_DONE;
		}
		siglongjmp(k_ctx->kthread_env, 1);
		return;
	}

	kthread_runq->cur_uthread = u_obj;
	if((u_obj->uthread_state == UTHREAD_INIT) && (uthread_init(u_obj)))
	{
		fprintf(stderr, "uthread_init failed on kthread(%d)\n", k_ctx->cpuid);
		exit(0);
	}
	u_obj->uthread_state = UTHREAD_RUNNING;
	gettimeofday(&(u_obj->start_time), NULL);
	/* Re-install the scheduling signal handlers */
	kthread_install_sighandler(SIGVTALRM, k_ctx->kthread_sched_timer);
	kthread_install_sighandler(SIGUSR1, k_ctx->kthread_sched_relay);
	/* Jump to the selected uthread context */
	siglongjmp(u_obj->uthread_env, 1);
	return;
}


/* For uthreads, we obtain a seperate stack by registering an alternate
 * stack for SIGUSR2 signal. Once the context is saved, we turn this 
 * into a regular stack for uthread (by using SS_DISABLE). */
static void uthread_context_func(int signo)
{
	uthread_struct_t *cur_uthread;
	kthread_runqueue_t *kthread_runq;

	kthread_runq = &(kthread_cpu_map[kthread_apic_id()]->krunqueue);

	printf("..... uthread_context_func .....\n");
	/* kthread->cur_uthread points to newly created uthread */
	if(!sigsetjmp(kthread_runq->cur_uthread->uthread_env,0))
	{
		/* In UTHREAD_INIT : saves the context and returns.
		 * Otherwise, continues execution. */
		/* DONT USE any locks here !! */
		assert(kthread_runq->cur_uthread->uthread_state == UTHREAD_INIT);
		kthread_runq->cur_uthread->uthread_state = UTHREAD_RUNNABLE;
		return;
	}

	/* UTHREAD_RUNNING : siglongjmp was executed. */
	cur_uthread = kthread_runq->cur_uthread;
	assert(cur_uthread->uthread_state == UTHREAD_RUNNING);
	/* Execute the uthread task */
	cur_uthread->uthread_func(cur_uthread->uthread_arg);
	cur_uthread->uthread_state = UTHREAD_DONE;

	uthread_schedule(&sched_find_best_uthread);
	return;
}

/**********************************************************************/
/* uthread creation */

extern kthread_runqueue_t *ksched_find_target(uthread_struct_t *);

extern int uthread_create(uthread_t *u_tid, int (*u_func)(void *), void *u_arg, uthread_group_t u_gid)
{
	kthread_runqueue_t *kthread_runq;
	uthread_struct_t *u_new;

	/* Signals used for cpu_thread scheduling */
	// kthread_block_signal(SIGVTALRM);
	// kthread_block_signal(SIGUSR1);

	/* create a new uthread structure and fill it */
	if(!(u_new = (uthread_struct_t *)MALLOCZ_SAFE(sizeof(uthread_struct_t))))
	{
		fprintf(stderr, "uthread mem alloc failure !!");
		exit(0);
	}

	u_new->uthread_state = UTHREAD_INIT;
	u_new->uthread_priority = DEFAULT_UTHREAD_PRIORITY;
	u_new->uthread_gid = u_gid;
	u_new->uthread_func = u_func;
	u_new->uthread_arg = u_arg;
	init_uthread_credits(u_new);
	init_cpu_time_tracking(u_new);

	/* Allocate new stack for uthread */
	u_new->uthread_stack.ss_flags = 0; /* Stack enabled for signal handling */
	if(!(u_new->uthread_stack.ss_sp = (void *)MALLOC_SAFE(UTHREAD_DEFAULT_SSIZE)))
	{
		fprintf(stderr, "uthread stack mem alloc failure !!");
		return -1;
	}
	u_new->uthread_stack.ss_size = UTHREAD_DEFAULT_SSIZE;


	{
		ksched_shared_info_t *ksched_info = &ksched_shared_info;

		gt_spin_lock(&ksched_info->ksched_lock);
		u_new->uthread_tid = ksched_info->kthread_tot_uthreads++;
		ksched_info->kthread_cur_uthreads++;
		gt_spin_unlock(&ksched_info->ksched_lock);
	}

	/* XXX: ksched_find_target should be a function pointer */
	kthread_runq = ksched_find_target(u_new);

	*u_tid = u_new->uthread_tid;
	/* Queue the uthread for target-cpu. Let target-cpu take care of initialization. */
	add_to_runqueue(kthread_runq->active_runq, &(kthread_runq->kthread_runqlock), u_new);


	/* WARNING : DONOT USE u_new WITHOUT A LOCK, ONCE IT IS ENQUEUED. */

	/* Resume with the old thread (with all signals enabled) */
	// kthread_unblock_signal(SIGVTALRM);
	// kthread_unblock_signal(SIGUSR1);

	return 0;
}

extern void gt_yield(int enable_yield) {
	if (!enable_yield) 
		return;
	
	kthread_context_t *k_ctx;
    kthread_runqueue_t *kthread_runq;
    uthread_struct_t *cur_uthread, *u_obj;

	// Get current kthread 
    k_ctx = kthread_cpu_map[kthread_apic_id()];
    kthread_runq = &(k_ctx->krunqueue);
    cur_uthread = kthread_runq->cur_uthread;
    
    if (!cur_uthread || cur_uthread->uthread_state != UTHREAD_RUNNING) {
        // SHOULDN'T HAPPEN
        // printf("[gt_yield] ERROR: Not in valid uthread context\n");
        return;
    }
	accumulate_cpu_time(cur_uthread); // accumalator

	
    // printf("\n[gt_yield] BEFORE YIELD - tid=%d gid=%d credits=%d\n", 
    //        cur_uthread->uthread_tid, cur_uthread->uthread_gid, 
    //        cur_uthread->uthread_credits); /* Print credits before yield */

	gt_spin_lock(&(kthread_runq->kthread_runqlock));
    kthread_runq->kthread_runqlock.holder = 0x05; 
    print_runq_stats(kthread_runq->active_runq, "ACTIVE_BEFORE_YIELD"); /* Print current active runqueue state */
	gt_spin_unlock(&(kthread_runq->kthread_runqlock));

	kthread_block_signal(SIGVTALRM);
    kthread_block_signal(SIGUSR1);
	cur_uthread->uthread_state = UTHREAD_RUNNABLE;

	// similar operation to uthread_schedule
	if (scheduler_type == SCHED_CREDIT) {
		u_obj = cur_uthread;

		struct timeval end_time, diff;
		gettimeofday(&end_time, NULL);
		long runtime_us = (end_time.tv_sec - u_obj->start_time.tv_sec) * 1000000 +
						(end_time.tv_usec - u_obj->start_time.tv_usec);
		
		int runtime_ms = runtime_us / 1000;
		printf("\n[gt_yield] credits tid=%d gid=%d, creds=%d, runtime_ms:%d, runtime_us:%lu\n",
				u_obj->uthread_tid, u_obj->uthread_gid, u_obj->uthread_credits, runtime_ms, runtime_us);
		if (runtime_ms > 0) {
			int old_creds = u_obj->uthread_credits;
			u_obj->uthread_credits -= runtime_ms;
			if (u_obj->uthread_credits < 0)
				u_obj->uthread_credits = 0;
			printf("\n[gt_yield] deducted credits tid=%d gid=%d, old_creds=%d, new_creds=%d\n",
				u_obj->uthread_tid, u_obj->uthread_gid, old_creds, u_obj->uthread_credits);
				
		}
	}

    // printf("[gt_yield] AFTER CREDIT DEDUCTION - tid=%d gid=%d credits=%d\n", 
    //        cur_uthread->uthread_tid, cur_uthread->uthread_gid, 
    //        cur_uthread->uthread_credits);
	
    cur_uthread->uthread_state = UTHREAD_RUNNABLE;
    
    add_to_runqueue(kthread_runq->active_runq, &(kthread_runq->kthread_runqlock), cur_uthread);
    
    gt_spin_lock(&(kthread_runq->kthread_runqlock));
    kthread_runq->kthread_runqlock.holder = 0x06;  /* gt_yield marker */
    kthread_runq->cur_uthread = NULL;
    print_runq_stats(kthread_runq->active_runq, "ACTIVE_AFTER_YIELD");
    gt_spin_unlock(&(kthread_runq->kthread_runqlock));

	// ADD sched section from uthread_schedule
	if (sigsetjmp(cur_uthread->uthread_env, 0)) {
        /* We return here when this thread is rescheduled */
        // printf("[gt_yield] RESUMED - tid=%d gid=%d credits=%d\n",
        //        cur_uthread->uthread_tid, cur_uthread->uthread_gid, 
        //        cur_uthread->uthread_credits);
        
        /* Re-enable scheduling signals */
        kthread_unblock_signal(SIGVTALRM);
        kthread_unblock_signal(SIGUSR1);
        return;
    }

	uthread_struct_t *next_uthread;
    if ((next_uthread = sched_find_best_uthread(kthread_runq))) {
        kthread_runq->cur_uthread = next_uthread;
        
        /* Initialize the thread if needed */
        if ((next_uthread->uthread_state == UTHREAD_INIT) && (uthread_init(next_uthread))) {
            fprintf(stderr, "uthread_init failed on kthread(%d)\n", k_ctx->cpuid);
            exit(0);
        }
        
        next_uthread->uthread_state = UTHREAD_RUNNING;
        gettimeofday(&(next_uthread->start_time), NULL);
        
        // printf("[gt_yield] SWITCHING TO - tid=%d gid=%d credits=%d\n",
        //        next_uthread->uthread_tid, next_uthread->uthread_gid, 
        //        next_uthread->uthread_credits);
        
        // Re-instal siganl handlers and jump to next thread 
        kthread_install_sighandler(SIGVTALRM, k_ctx->kthread_sched_timer);
        kthread_install_sighandler(SIGUSR1, k_ctx->kthread_sched_relay);
        
        siglongjmp(next_uthread->uthread_env, 1);
    } else {
        // no more threads to run - return to kthread main loop
        if (ksched_shared_info.kthread_tot_uthreads && !ksched_shared_info.kthread_cur_uthreads) {
            fprintf(stderr, "Quitting kthread (%d) after gt_yield\n", k_ctx->cpuid);
            k_ctx->kthread_flags |= KTHREAD_DONE;
        }
        siglongjmp(k_ctx->kthread_env, 1);
    }

}

#if 0
/**********************************************************************/
kthread_runqueue_t kthread_runqueue;
kthread_runqueue_t *kthread_runq = &kthread_runqueue;
sigjmp_buf kthread_env;

/* Main Test */
typedef struct uthread_arg
{
	int num1;
	int num2;
	int num3;
	int num4;	
} uthread_arg_t;

#define NUM_THREADS 10
static int func(void *arg);

int main()
{
	uthread_struct_t *uthread;
	uthread_t u_tid;
	uthread_arg_t *uarg;

	int inx;

	/* XXX: Put this lock in kthread_shared_info_t */
	gt_spinlock_init(&uthread_group_penalty_lock);

	/* spin locks are initialized internally */
	kthread_init_runqueue(kthread_runq);

	for(inx=0; inx<NUM_THREADS; inx++)
	{
		uarg = (uthread_arg_t *)MALLOC_SAFE(sizeof(uthread_arg_t));
		uarg->num1 = inx;
		uarg->num2 = 0x33;
		uarg->num3 = 0x55;
		uarg->num4 = 0x77;
		uthread_create(&u_tid, func, uarg, (inx % MAX_UTHREAD_GROUPS));
	}

	kthread_init_vtalrm_timeslice();
	kthread_install_sighandler(SIGVTALRM, kthread_sched_vtalrm_handler);
	if(sigsetjmp(kthread_env, 0) > 0)
	{
		/* XXX: (TODO) : uthread cleanup */
		exit(0);
	}

	uthread_schedule(&ksched_priority);
	return(0);
}

static int func(void *arg)
{
	unsigned int count;
#define u_info ((uthread_arg_t *)arg)
	printf("Thread %d created\n", u_info->num1);
	count = 0;
	while(count <= 0xffffff)
	{
		if(!(count % 5000000))
			printf("uthread(%d) => count : %d\n", u_info->num1, count);
		count++;
	}
#undef u_info
	return 0;
}
#endif
