/*
 * Copyright 2019, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <autoconf.h>
#include <sel4benchirquser/gen_config.h>
#include <stdio.h>
#include <sel4runtime.h>
#include <muslcsys/vsyscall.h>
#include <utils/attribute.h>

#include <sel4platsupport/timer.h>
#include <sel4platsupport/irq.h>
#include <utils/time.h>

#include <platsupport/irq.h>
#include <platsupport/ltimer.h>

#include <sel4bench/arch/sel4bench.h>
#include <sel4bench/kernel_logging.h>
#include <sel4bench/logging.h>

#include <benchmark.h>
#include <irq.h>

#define INTERRUPT_PERIOD_NS (10 * NS_IN_MS)
#define DACHEPOLLUTION_SIZE 64000

int *array;
int **ptrs;

void init_dcache_pollution(void) {
    array = malloc(DACHEPOLLUTION_SIZE * sizeof(int));
    ptrs = malloc(DACHEPOLLUTION_SIZE * sizeof(int *));
}

void pollute_dcache(void) {
    for (int i = 0; i < DACHEPOLLUTION_SIZE; i++) {
        ptrs[i] = &array[i];
    }

    // shuffle the pointers
    for (int i = 0; i < DACHEPOLLUTION_SIZE; i++) {
        int j = rand() % DACHEPOLLUTION_SIZE;
        int *tmp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = tmp;
    }

    // access the array through the shuffled pointers
    for (int i = 0; i < DACHEPOLLUTION_SIZE; i++) {
        *ptrs[i] = i;
    }
}

void pollute_icache(void) {
    __asm__(".rept 32000\n\t"
            "nop\n\t"
            ".endr");
}


void abort(void)
{
    benchmark_finished(EXIT_FAILURE);
}

void spinner_fn(int argc, char **argv)
{
    // sel4bench_init();
    if (argc != 1) {
        abort();
    }

    volatile ccnt_t *current_time = (volatile ccnt_t *) atol(argv[0]);

    while (1) {
        /* just take the low bits so the reads are atomic */
        SEL4BENCH_READ_CCNT(*current_time);
        // ZF_LOGE("current_time: %ld", (seL4_Word) * current_time);
    }
}

/* ep for ticker to Send on when done */
static seL4_CPtr done_ep;
/* ntfn for ticker to wait for timer irqs on */
static seL4_CPtr timer_signal;
/* initialised IRQ interface */
static ps_irq_ops_t *irq_ops;
/* ntfn_id of the timer notification provided to the IRQ interface */
static ntfn_id_t timer_ntfn_id;

void ticker_fn(ccnt_t *results, volatile ccnt_t *current_time)
{
    seL4_Word start, end_low;
    ccnt_t end;
    seL4_Word badge;

    for (int i = 0; i < 1; i++) {
        /* wait for irq */
        seL4_Wait(timer_signal, &badge);
        // ZF_LOGE("Got IRQ");
        /* record result */
        SEL4BENCH_READ_CCNT(end);
        sel4platsupport_irq_handle(irq_ops, timer_ntfn_id, badge);
        end_low = (seL4_Word) end;
        start = (seL4_Word) * current_time;
        // ZF_LOGE("start: %ld, end: %ld", start, end_low);
        results[i] = end_low - start;
        // ZF_LOGE("result: %ld", results[i]);
    }

    seL4_Send(done_ep, seL4_MessageInfo_new(0, 0, 0, 0));
}

static env_t *env;

void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY) init_env(void)
{
    static size_t object_freq[seL4_ObjectTypeCount] = {
        [seL4_TCBObject] = 2,
        [seL4_EndpointObject] = 1,
#ifdef CONFIG_KERNEL_MCS
        [seL4_SchedContextObject] = 2,
        [seL4_ReplyObject] = 2
#endif
    };

    env = benchmark_get_env(
              sel4runtime_argc(),
              sel4runtime_argv(),
              sizeof(irquser_results_t),
              object_freq
          );
}

int main(int argc, char **argv)
{
    irquser_results_t *results;
    vka_object_t endpoint = {0};

    benchmark_init_timer(env);
    results = (irquser_results_t *) env->results;

    if (vka_alloc_endpoint(&env->slab_vka, &endpoint) != 0) {
        ZF_LOGF("Failed to allocate endpoint\n");
    }

    /* set up globals */
    done_ep = endpoint.cptr;
    timer_signal = env->ntfn.cptr;
    irq_ops = &env->io_ops.irq_ops;
    timer_ntfn_id = env->ntfn_id;

    init_dcache_pollution();
    pollute_dcache();
    pollute_icache();

    int error = ltimer_reset(&env->ltimer);
    ZF_LOGF_IF(error, "Failed to start timer");

    // error = ltimer_set_timeout(&env->ltimer, INTERRUPT_PERIOD_NS, TIMEOUT_PERIODIC);
    // ZF_LOGF_IF(error, "Failed to configure timer");
    
    sel4bench_init();

    sel4utils_thread_t ticker, spinner;

    /* measurement overhead */
    ccnt_t start, end;
    for (int i = 0; i < N_RUNS; i++) {
        SEL4BENCH_READ_CCNT(start);
        SEL4BENCH_READ_CCNT(end);
        results->overheads[i] = end - start;
    }

    /* create a frame for the shared time variable so we can share it between processes */
    ccnt_t *local_current_time = (ccnt_t *) vspace_new_pages(&env->vspace, seL4_AllRights, 1, seL4_PageBits);
    if (local_current_time == NULL) {
        ZF_LOGF("Failed to allocate page");
    }

    /* first run the benchmark between two threads in the current address space */
    benchmark_configure_thread(env, endpoint.cptr, seL4_MaxPrio - 1, "ticker", &ticker);
    benchmark_configure_thread(env, endpoint.cptr, seL4_MaxPrio - 2, "spinner", &spinner);

    // ZF_LOGE("Starting spinner");
    char strings[1][WORD_STRING_SIZE];
    char *spinner_argv[1];
    sel4utils_create_word_args(strings, spinner_argv, 1, (seL4_Word) local_current_time);
    error = sel4utils_start_thread(&spinner, (sel4utils_thread_entry_fn) spinner_fn, (void *) 1, (void *) spinner_argv,
                                   true);

    for (int i = 0; i < N_RUNS; i++) {
        // ZF_LOGE("Starting run %d", i);
        
        // ZF_LOGE("Restarting timer");
        error = ltimer_set_timeout(&env->ltimer, INTERRUPT_PERIOD_NS, TIMEOUT_PERIODIC);
        ZF_LOGF_IF(error, "Failed to configure timer");

        // ZF_LOGE("Starting ticker");
        error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn, (void *) (results->thread_results + i),
                                   (void *) local_current_time, true);
        if (error) {
            ZF_LOGF("Failed to start ticker");
        }

        // seL4_DebugDumpScheduler();

        benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

        // ZF_LOGE("Stopping ticker");
        error = seL4_TCB_Suspend(ticker.tcb.cptr);
        assert(error == seL4_NoError);

        // ZF_LOGE("Resetting timer");
        int error = ltimer_reset(&env->ltimer);
        ZF_LOGF_IF(error, "Failed to start timer");
        
        // ZF_LOGE("Polluting caches");
        pollute_dcache(); 
        pollute_icache();
    }

    /* now run the benchmark again, but run the spinner in another address space */
    
    //  TODO: Fix this
    ltimer_set_timeout(&env->ltimer, INTERRUPT_PERIOD_NS, TIMEOUT_PERIODIC);
    // ZF_LOGE("Stopping spinner");
    error = seL4_TCB_Suspend(spinner.tcb.cptr);
    assert(error == seL4_NoError);

    /* restart ticker */
    error = sel4utils_start_thread(&ticker, (sel4utils_thread_entry_fn) ticker_fn, (void *) results->process_results,
                                   (void *) local_current_time, true);
    assert(!error);

    sel4utils_process_t spinner_process;
    benchmark_shallow_clone_process(env, &spinner_process, seL4_MaxPrio - 2, spinner_fn, "spinner");

    /* share the current time variable with the spinner process */
    void *current_time_remote = vspace_share_mem(&env->vspace, &spinner_process.vspace,
                                                 (void *) local_current_time, 1, seL4_PageBits,
                                                 seL4_AllRights, true);
    assert(current_time_remote != NULL);

    /* start the spinner process */
    sel4utils_create_word_args(strings, spinner_argv, 1, (seL4_Word) current_time_remote);
    error = benchmark_spawn_process(&spinner_process, &env->slab_vka, &env->vspace, 1, spinner_argv, 1);
    if (error) {
        ZF_LOGF("Failed to start spinner process");
    }

    benchmark_wait_children(endpoint.cptr, "child of irq-user", 1);

    /* done -> results are stored in shared memory so we can now return */
    benchmark_finished(EXIT_SUCCESS);
    return 0;
}
