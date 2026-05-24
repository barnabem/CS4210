#ifndef __GT_INCLUDE_H
#define __GT_INCLUDE_H

#include "gt_signal.h"
#include "gt_spinlock.h"
#include "gt_tailq.h"
#include "gt_bitops.h"

#include "gt_uthread.h"
#include "gt_pq.h"
#include "gt_kthread.h"

/////// USER DEFINED FOR PROJECT 1 ///////
#define SCHED_PRIORITY  0
#define SCHED_CREDIT    1
#define LOG             1
// #define GT_THREADS      1
extern int scheduler_type;
extern int load_balancing_enabled;
static const unsigned int CREDIT_GROUPS[] = { 25, 50, 75, 100 };
#define NUM_CREDIT_GROUPS (sizeof(CREDIT_GROUPS)/sizeof(CREDIT_GROUPS[0]))
//////////////////////////////////////////


#endif
