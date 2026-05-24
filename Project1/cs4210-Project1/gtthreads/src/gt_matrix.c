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
#include <string.h>

#include "gt_include.h"


#define ROWS 50
#define COLS ROWS
#define SIZE COLS

#define NUM_CPUS 3
#define NUM_GROUPS NUM_CPUS
#define NUM_CREDIT_LEVELS 4  
#define NUM_MATRIX_SIZES 4
static const int MATRIX_SIZES[NUM_MATRIX_SIZES] = {32, 64, 128, 256};   
#define THREADS_PER_COMBO 8  
#define PER_GROUP_COLS (SIZE/NUM_GROUPS)

#define NUM_THREADS 27
#define PER_THREAD_ROWS (SIZE/NUM_THREADS)

#define YIELD_INTERVAL 25  // Yield every X rows
#define YIELD_ENABLE 0 // Enable yield

// globals
int scheduler_type = SCHED_PRIORITY;
int load_balancing_enabled = 0;


/* A[SIZE][SIZE] X B[SIZE][SIZE] = C[SIZE][SIZE]
 * Let T(g, t) be thread 't' in group 'g'. 
 * T(g, t) is responsible for multiplication : 
 * A(rows)[(t-1)*SIZE -> (t*SIZE - 1)] X B(cols)[(g-1)*SIZE -> (g*SIZE - 1)] */
typedef struct {
    int rows;
    int cols;
    int **m;   // pointer to an array of row pointers
} matrix_t;


typedef struct __uthread_arg
{
	matrix_t *_A, *_B, *_C;
	unsigned int reserved0;

	unsigned int tid;
	unsigned int gid;
	int n; // matrix size (square matrix so N x N)
	int start_row; /* start_row -> (start_row + PER_THREAD_ROWS) */
	int start_col; /* start_col -> (start_col + PER_GROUP_COLS) */

}uthread_arg_t;

struct timeval tv1;

static void generate_matrix(matrix_t *mat, int val)
{

	int i,j;
	mat->rows = SIZE;
	mat->cols = SIZE;
	for(i = 0; i < mat->rows;i++)
		for( j = 0; j < mat->cols; j++ )
		{
			mat->m[i][j] = val;
		}
	return;
}

static void print_matrix(matrix_t *mat)
{
	int i, j;

	for(i=0;i<mat->rows;i++)
	{
		for(j=0;j<mat->cols;j++)
			printf(" %d ",mat->m[i][j]);
		printf("\n");
	}

	return;
}

extern int uthread_create(uthread_t *, void *, void *, uthread_group_t);

static void * uthread_mulmat(void *p)
{
	int i, j, k;
	// int start_row, end_row;
	// int start_col, end_col;
	int n = 0;
	unsigned int cpuid;
	struct timeval tv2;

#define ptr ((uthread_arg_t *)p)

	i=0; j= 0; k=0;
	// start_row = ptr->start_row;
	// end_row = (ptr->start_row + PER_THREAD_ROWS);
	n = ptr->n;

#ifdef GT_GROUP_SPLIT
	start_col = ptr->start_col;
	end_col = (ptr->start_col + PER_THREAD_ROWS);
#endif

#ifdef GT_THREADS
	cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;
	fprintf(stderr, "\nThread(id:%d, group:%d, cpu:%d) started",ptr->tid, ptr->gid, cpuid);
#else
	cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;
	if (!LOG)
		fprintf(stderr, "\nThread(id:%d, group:%d, cpu:%d) started",ptr->tid, ptr->gid, cpuid);
#endif
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int sum = 0;
            for (int k = 0; k < n; k++) {
                sum += ptr->_A->m[i][k] * ptr->_B->m[k][j];
            }
            ptr->_C->m[i][j] = sum;
        }
		// Yield periodically, not after every row
		if ((i + 1) % YIELD_INTERVAL == 0) {
			gt_yield(YIELD_ENABLE);
			
			#ifndef GT_THREADS
				unsigned int new_cpuid = kthread_cpu_map[kthread_apic_id()]->cpuid;
				if (new_cpuid != cpuid) {
					printf("    *** Thread(id:%d) MOVED from CPU:%d to CPU:%d ***\n", 
						ptr->tid, cpuid, new_cpuid);
					cpuid = new_cpuid;
				}
			#endif
		}
    }
#ifdef GT_THREADS
	fprintf(stderr, "\nThread(id:%d, group:%d, cpu:%d) finished (TIME : %lu s and %lu us)",
			ptr->tid, ptr->gid, cpuid, (tv2.tv_sec - tv1.tv_sec), (tv2.tv_usec - tv1.tv_usec));
#else
	// gettimeofday(&tv2,NULL);
	if (LOG) {
		fprintf(stderr, "c%d_m_%d,%d,",
			CREDIT_GROUPS[ptr->gid], ptr->n, ptr->tid);
	} else {
		fprintf(stderr, "\nThread(id:%d, group:%d, mat_size: %d, ",
			ptr->tid, ptr->gid, ptr->n);
	}
	
	// gettimeofday(&tv2,NULL);
    // fprintf(stderr, "\nThread(id:%d, group:%d) finished (TOTAL TIME: %lu s and %lu us; CPU TIME: %lu s and %lu us)",
    //         ptr->tid, ptr->gid, 
    //         (tv2.tv_sec - tv1.tv_sec), (tv2.tv_usec - tv1.tv_usec),
    //         ptr->final_cpu_time.tv_sec, ptr->final_cpu_time.tv_usec);
#endif

#undef ptr
	return 0;
}

// matrix_t A, B, C;

// static void init_matrices()
// {
// 	generate_matrix(&A, 1);
// 	generate_matrix(&B, 1);
// 	generate_matrix(&C, 0);

// 	return;
// }

static matrix_t *alloc_matrix(int n, int val) {
    int i, j;
    matrix_t *mat = malloc(sizeof(matrix_t));
    mat->rows = n;
    mat->cols = n;

    mat->m = malloc(n * sizeof(int*)); // malloc for ptr with num rows

    // Alloc each row within ptr
    for (i = 0; i < n; i++) {
        mat->m[i] = malloc(n * sizeof(int));
        for (j = 0; j < n; j++) {
            mat->m[i][j] = val;   // initialize with value
        }
    }

    return mat;
}

void free_matrix(matrix_t *mat) { // free matrix and pointers when done
    for (int i = 0; i < mat->rows; i++) {
        free(mat->m[i]);
    }
    free(mat->m);
    free(mat);
}



void parse_args(int argc, const char* argv[])
{
	int inx;

	for(inx=0; inx<argc; inx++)
	{
		if(argv[inx][0]=='-')
		{
			if(!strcmp(&argv[inx][1], "lb"))
			{
				load_balancing_enabled = 1;
				printf("enable load balancing\n");
			}
			else if(!strcmp(&argv[inx][1], "s"))
			{
				//TODO: add different types of scheduler
				inx++;
				if(!strcmp(&argv[inx][0], "0"))
				{
					scheduler_type = SCHED_PRIORITY;
					printf("use priority scheduler\n");
				}
				else if(!strcmp(&argv[inx][0], "1"))
				{
					scheduler_type = SCHED_CREDIT;
					printf("use credit scheduler\n");
				}
			}
		}
	}

	return;
}



uthread_arg_t uargs[NUM_THREADS];
uthread_t utids[NUM_THREADS];

int main(int argc, char const *argv[])
{
	uthread_arg_t *uarg;
    int thread_idx = 0;
    int credit_idx, size_idx, combo_thread;

	parse_args(argc, argv);

	gtthread_app_init();

	// init_matrices(); (no need for global matrices)

	gettimeofday(&tv1,NULL);

	for(int inx=0; inx<NUM_THREADS; inx++)
	{
		uarg = &uargs[inx];
		uarg->n = SIZE; // size of matrices
		uarg->_A = alloc_matrix(uarg->n, 1);
		uarg->_B = alloc_matrix(uarg->n, 1);
		uarg->_C = alloc_matrix(uarg->n, 0);

		uarg->tid = inx;

		uarg->gid = (inx % NUM_GROUPS);

		uarg->start_row = (inx * PER_THREAD_ROWS);
#ifdef GT_GROUP_SPLIT
		/* Wanted to split the columns by groups !!! */
		uarg->start_col = (uarg->gid * PER_GROUP_COLS);
#endif

		uthread_create(&utids[inx], uthread_mulmat, uarg, uarg->gid);
	}
	// FOR EXPERIMENT IN REPORT
	// for(credit_idx = NUM_CREDIT_LEVELS-1; credit_idx >= 0; credit_idx--)
	// {
	// 	for(size_idx = 0; size_idx < NUM_MATRIX_SIZES; size_idx++)
	// 	{
	// 		int current_credit = CREDIT_GROUPS[credit_idx];
	// 		int current_matrix_size = MATRIX_SIZES[size_idx];
			
	// 		// printf("Creating %d threads with credits=%d, matrix_size=%d\n", 
	// 		// 	THREADS_PER_COMBO, current_credit, current_matrix_size);
			
	// 		// Create THREADS_PER_COMBO threads for this combination
	// 		for(combo_thread = 0; combo_thread < THREADS_PER_COMBO; combo_thread++)
	// 		{
	// 			uarg = &uargs[thread_idx];
	// 			uarg->n = current_matrix_size;
	// 			uarg->_A = alloc_matrix(uarg->n, 1);
	// 			uarg->_B = alloc_matrix(uarg->n, 1);
	// 			uarg->_C = alloc_matrix(uarg->n, 0);
	// 			uarg->tid = thread_idx;
	// 			uarg->gid = credit_idx;  // use credit_idx as group ID (0-3)

	// 			// Optional: set start row/col if needed for partitioning
	// 			uarg->start_row = 0;  // Each thread works on its own complete matrix
	// 			uarg->start_col = 0;

	// 			// printf("  Thread %d: gid=%d, credits=%d, matrix_size=%d\n", 
	// 			// 	thread_idx, uarg->gid, current_credit, current_matrix_size);

	// 			uthread_create(&utids[thread_idx], uthread_mulmat, uarg, uarg->gid);
				
	// 			thread_idx++;
	// 		}
	// 	}
	// }

	gtthread_app_exit();

	// for each thread kill matrices
	for (int inx = 0; inx < NUM_THREADS; inx++) {
		free_matrix(uargs[inx]._A);
		free_matrix(uargs[inx]._B);
		free_matrix(uargs[inx]._C);
	}


	// print_matrix(&C);
	// fprintf(stderr, "********************************");
	return(0);
}

