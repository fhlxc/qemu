/*
 * Blockjob transactions tests
 *
 * Copyright Red Hat, Inc. 2015
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "block/blockjob_int.h"
#include "sysemu/block-backend.h"

typedef struct {
    BlockJob common;
    unsigned int iterations;
    bool use_timer;
    int rc;
    int *result;
} TestBlockJob;

static void test_block_job_complete(Job *job, void *opaque)
{
    BlockJob *bjob = container_of(job, BlockJob, job);
    BlockDriverState *bs = blk_bs(bjob->blk);
    int rc = (intptr_t)opaque;

    if (job_is_cancelled(job)) {
        rc = -ECANCELED;
    }

    job_completed(job, rc, NULL);
    bdrv_unref(bs);
}

static void coroutine_fn test_block_job_run(void *opaque)
{
    TestBlockJob *s = opaque;
    BlockJob *job = &s->common;

    while (s->iterations--) {
        if (s->use_timer) {
            job_sleep_ns(&job->job, 0);
        } else {
            job_yield(&job->job);
        }

        if (job_is_cancelled(&job->job)) {
            break;
        }
    }

    job_defer_to_main_loop(&job->job, test_block_job_complete,
                           (void *)(intptr_t)s->rc);
}

typedef struct {
    TestBlockJob *job;
    int *result;
} TestBlockJobCBData;

static void test_block_job_cb(void *opaque, int ret)
{
    TestBlockJobCBData *data = opaque;
    if (!ret && job_is_cancelled(&data->job->common.job)) {
        ret = -ECANCELED;
    }
    *data->result = ret;
    g_free(data);
}

static const BlockJobDriver test_block_job_driver = {
    .job_driver = {
        .instance_size = sizeof(TestBlockJob),
        .free          = block_job_free,
        .user_resume   = block_job_user_resume,
        .drain         = block_job_drain,
        .start         = test_block_job_run,
    },
};

/* Create a block job that completes with a given return code after a given
 * number of event loop iterations.  The return code is stored in the given
 * result pointer.
 *
 * The event loop iterations can either be handled automatically with a 0 delay
 * timer, or they can be stepped manually by entering the coroutine.
 */
static BlockJob *test_block_job_start(unsigned int iterations,
                                      bool use_timer,
                                      int rc, int *result, JobTxn *txn)
{
    BlockDriverState *bs;
    TestBlockJob *s;
    TestBlockJobCBData *data;
    static unsigned counter;
    char job_id[24];

    data = g_new0(TestBlockJobCBData, 1);

    bs = bdrv_open("null-co://", NULL, NULL, 0, &error_abort);
    g_assert_nonnull(bs);

    snprintf(job_id, sizeof(job_id), "job%u", counter++);
    s = block_job_create(job_id, &test_block_job_driver, txn, bs,
                         0, BLK_PERM_ALL, 0, JOB_DEFAULT,
                         test_block_job_cb, data, &error_abort);
    s->iterations = iterations;
    s->use_timer = use_timer;
    s->rc = rc;
    s->result = result;
    data->job = s;
    data->result = result;
    return &s->common;
}

static void test_single_job(int expected)
{
    BlockJob *job;
    JobTxn *txn;
    int result = -EINPROGRESS;

    txn = job_txn_new();
    job = test_block_job_start(1, true, expected, &result, txn);
    job_start(&job->job);

    if (expected == -ECANCELED) {
        job_cancel(&job->job, false);
    }

    while (result == -EINPROGRESS) {
        aio_poll(qemu_get_aio_context(), true);
    }
    g_assert_cmpint(result, ==, expected);

    job_txn_unref(txn);
}

static void test_single_job_success(void)
{
    test_single_job(0);
}

static void test_single_job_failure(void)
{
    test_single_job(-EIO);
}

static void test_single_job_cancel(void)
{
    test_single_job(-ECANCELED);
}

static void test_pair_jobs(int expected1, int expected2)
{
    BlockJob *job1;
    BlockJob *job2;
    JobTxn *txn;
    int result1 = -EINPROGRESS;
    int result2 = -EINPROGRESS;

    txn = job_txn_new();
    job1 = test_block_job_start(1, true, expected1, &result1, txn);
    job2 = test_block_job_start(2, true, expected2, &result2, txn);
    job_start(&job1->job);
    job_start(&job2->job);

    /* Release our reference now to trigger as many nice
     * use-after-free bugs as possible.
     */
    job_txn_unref(txn);

    if (expected1 == -ECANCELED) {
        job_cancel(&job1->job, false);
    }
    if (expected2 == -ECANCELED) {
        job_cancel(&job2->job, false);
    }

    while (result1 == -EINPROGRESS || result2 == -EINPROGRESS) {
        aio_poll(qemu_get_aio_context(), true);
    }

    /* Failure or cancellation of one job cancels the other job */
    if (expected1 != 0) {
        expected2 = -ECANCELED;
    } else if (expected2 != 0) {
        expected1 = -ECANCELED;
    }

    g_assert_cmpint(result1, ==, expected1);
    g_assert_cmpint(result2, ==, expected2);
}

static void test_pair_jobs_success(void)
{
    test_pair_jobs(0, 0);
}

static void test_pair_jobs_failure(void)
{
    /* Test both orderings.  The two jobs run for a different number of
     * iterations so the code path is different depending on which job fails
     * first.
     */
    test_pair_jobs(-EIO, 0);
    test_pair_jobs(0, -EIO);
}

static void test_pair_jobs_cancel(void)
{
    test_pair_jobs(-ECANCELED, 0);
    test_pair_jobs(0, -ECANCELED);
}

static void test_pair_jobs_fail_cancel_race(void)
{
    BlockJob *job1;
    BlockJob *job2;
    JobTxn *txn;
    int result1 = -EINPROGRESS;
    int result2 = -EINPROGRESS;

    txn = job_txn_new();
    job1 = test_block_job_start(1, true, -ECANCELED, &result1, txn);
    job2 = test_block_job_start(2, false, 0, &result2, txn);
    job_start(&job1->job);
    job_start(&job2->job);

    job_cancel(&job1->job, false);

    /* Now make job2 finish before the main loop kicks jobs.  This simulates
     * the race between a pending kick and another job completing.
     */
    job_enter(&job2->job);
    job_enter(&job2->job);

    while (result1 == -EINPROGRESS || result2 == -EINPROGRESS) {
        aio_poll(qemu_get_aio_context(), true);
    }

    g_assert_cmpint(result1, ==, -ECANCELED);
    g_assert_cmpint(result2, ==, -ECANCELED);

    job_txn_unref(txn);
}

int main(int argc, char **argv)
{
    qemu_init_main_loop(&error_abort);
    bdrv_init();

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/single/success", test_single_job_success);
    g_test_add_func("/single/failure", test_single_job_failure);
    g_test_add_func("/single/cancel", test_single_job_cancel);
    g_test_add_func("/pair/success", test_pair_jobs_success);
    g_test_add_func("/pair/failure", test_pair_jobs_failure);
    g_test_add_func("/pair/cancel", test_pair_jobs_cancel);
    g_test_add_func("/pair/fail-cancel-race", test_pair_jobs_fail_cancel_race);
    return g_test_run();
}
