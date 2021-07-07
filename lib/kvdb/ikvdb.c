/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2021 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_ikvdb
#define MTF_MOCK_IMPL_kvs

#include <hse/hse.h>
#include <hse/flags.h>

#include <hse_util/hse_err.h>
#include <hse_util/event_counter.h>
#include <hse_util/string.h>
#include <hse_util/page.h>
#include <hse_util/seqno.h>
#include <hse_util/darray.h>
#include <hse_util/rest_api.h>
#include <hse_util/log2.h>
#include <hse_util/atomic.h>
#include <hse_util/vlb.h>
#include <hse_util/compression_lz4.h>
#include <hse_util/token_bucket.h>
#include <hse_util/xrand.h>
#include <hse_util/bkv_collection.h>

#include <hse_ikvdb/config.h>
#include <hse_ikvdb/argv.h>
#include <hse_ikvdb/ikvdb.h>
#include <hse_ikvdb/kvdb_health.h>
#include <hse_ikvdb/kvs.h>
#include <hse_ikvdb/c0.h>
#include <hse_ikvdb/c0sk.h>
#include <hse_ikvdb/c0sk_perfc.h>
#include <hse_ikvdb/lc.h>
#include <hse_ikvdb/cn.h>
#include <hse_ikvdb/cn_kvdb.h>
#include <hse_ikvdb/cn_perfc.h>
#include <hse_ikvdb/ctxn_perfc.h>
#include <hse_ikvdb/kvdb_perfc.h>
#include <hse_ikvdb/cndb.h>
#include <hse_ikvdb/kvdb_ctxn.h>
#include <hse_ikvdb/c0snr_set.h>
#include <hse_ikvdb/limits.h>
#include <hse_ikvdb/key_hash.h>
#include <hse_ikvdb/diag_kvdb.h>
#include <hse_ikvdb/c0_kvset.h>
#include <hse_ikvdb/csched.h>
#include <hse_ikvdb/throttle.h>
#include <hse_ikvdb/throttle_perfc.h>
#include <hse_ikvdb/rparam_debug_flags.h>
#include <hse_ikvdb/mclass_policy.h>
#include <hse_ikvdb/kvdb_cparams.h>
#include <hse_ikvdb/kvdb_dparams.h>
#include <hse_ikvdb/kvdb_rparams.h>
#include <hse_ikvdb/kvs_cparams.h>
#include <hse_ikvdb/kvs_rparams.h>
#include <hse_ikvdb/wal.h>
#include "kvdb_omf.h"

#include "kvdb_log.h"
#include "kvdb_kvs.h"
#include "viewset.h"
#include "kvdb_keylock.h"

#include <mpool/mpool.h>

#include <xxhash.h>
#ifdef WITH_CJSON_FROM_SUBPROJECT
#include <cJSON.h>
#else
#include <cjson/cJSON.h>
#endif
#include <bsd/libutil.h>

#include "kvdb_rest.h"

/* tls_vbuf[] is a thread-local buffer used as a compression output buffer
 * by ikvdb_kvs_put() and for small direct reads by kvset_lookup_val().
 */
thread_local char tls_vbuf[32 * 1024] HSE_ALIGNED(PAGE_SIZE);
const size_t      tls_vbufsz = sizeof(tls_vbuf);

struct perfc_set kvdb_pkvdbl_pc HSE_READ_MOSTLY;
struct perfc_set kvdb_pc        HSE_READ_MOSTLY;

struct perfc_set kvdb_metrics_pc HSE_READ_MOSTLY;
struct perfc_set c0_metrics_pc   HSE_READ_MOSTLY;

static_assert((sizeof(uintptr_t) == sizeof(u64)), "code relies on pointers being 64-bits in size");

#define ikvdb_h2r(handle) container_of(handle, struct ikvdb_impl, ikdb_handle)

struct ikvdb {
};

/* Max buckets in ctxn cache.  Must be prime for best results.
 */
#define KVDB_CTXN_BKT_MAX (17)

/* Simple fixed-size stack for caching ctxn objects.
 */
struct kvdb_ctxn_bkt {
    spinlock_t kcb_lock HSE_ALIGNED(SMP_CACHE_BYTES * 2);
    uint                kcb_ctxnc;
    struct kvdb_ctxn *  kcb_ctxnv[15];
};

/**
 * struct ikvdb_impl - private representation of a kvdb
 * @ikdb_handle:        handle for users of struct ikvdb_impl's
 * @ikdb_rdonly:        bool indicating read-only mode
 * @ikdb_work_stop:
 * @ikdb_ctxn_set:      kvdb transaction set
 * @ikdb_ctxn_op:       transaction performance counters
 * @ikdb_keylock:       handle to the KVDB keylock
 * @ikdb_c0sk:          c0sk handle
 * @ikdb_health:
 * @ikdb_mp:            mpool handle
 * @ikdb_log:           KVDB log handle
 * @ikdb_cndb:          CNDB handle
 * @ikdb_workqueue:
 * @ikdb_curcnt:        number of active cursors (lazily updated)
 * @ikdb_curcnt_max:    maximum number of active cursors
 * @ikdb_cur_ticket:    ticket lock ticket dispenser (serializes ikvdb_cur_list access)
 * @ikdb_cur_serving:   ticket lock "now serving" number
 * @ikdb_seqno:         current sequence number for the struct ikvdb
 * @ikdb_cur_list:      list of cursors holding the cursor horizon
 * @ikdb_cur_horizon:   oldest seqno in cursor ikdb_cur_list
 * @ikdb_rp:            KVDB run time params
 * @ikdb_ctxn_cache:    ctxn cache
 * @ikdb_lock:          protects ikdb_kvs_vec/ikdb_kvs_cnt writes
 * @ikdb_kvs_cnt:       number of KVSes in ikdb_kvs_vec
 * @ikdb_kvs_vec:       vector of KVDB KVSes
 * @ikdb_maint_work:
 * @ikdb_profile:       hse params stored as profile
 * @ikdb_cndb_oid1:
 * @ikdb_cndb_oid2:
 * @ikdb_home:          KVDB home
 *
 * Note:  The first group of fields are read-mostly and some of them are
 * heavily concurrently accessed, hence they live in the first cache line.
 * Only add a new field to this group if it is read-mostly and would not push
 * the first field of %ikdb_health out of the first cache line.  Similarly,
 * the group of fields which contains %ikdb_seqno is heavily concurrently
 * accessed and heavily modified. Only add a new field to this group if it
 * will be accessed just before or after accessing %ikdb_seqno.
 */
struct ikvdb_impl {
    struct ikvdb          ikdb_handle;
    bool                  ikdb_rdonly;
    bool                  ikdb_work_stop;
    struct kvdb_ctxn_set *ikdb_ctxn_set;
    struct c0snr_set *    ikdb_c0snr_set;
    struct perfc_set      ikdb_ctxn_op;
    struct kvdb_keylock * ikdb_keylock;
    struct c0sk *         ikdb_c0sk;
    struct lc *           ikdb_lc;
    struct kvdb_health    ikdb_health;

    struct throttle ikdb_throttle;

    struct wal              *ikdb_wal;
    struct kvdb_callback     ikdb_wal_cb;
    struct csched *          ikdb_csched;
    struct cn_kvdb *         ikdb_cn_kvdb;
    struct mpool *           ikdb_mp;
    struct kvdb_log *        ikdb_log;
    struct cndb *            ikdb_cndb;
    struct workqueue_struct *ikdb_workqueue;
    struct viewset *         ikdb_txn_viewset;
    struct viewset *         ikdb_cur_viewset;

    struct tbkt ikdb_tb HSE_ALIGNED(SMP_CACHE_BYTES * 2);

    u64 ikdb_tb_burst;
    u64 ikdb_tb_rate;

    u64        ikdb_tb_dbg;
    u64        ikdb_tb_dbg_next;
    atomic64_t ikdb_tb_dbg_ops;
    atomic64_t ikdb_tb_dbg_bytes;
    atomic64_t ikdb_tb_dbg_sleep_ns;

    atomic_t ikdb_curcnt HSE_ALIGNED(SMP_CACHE_BYTES * 2);
    u32                  ikdb_curcnt_max;

    atomic64_t ikdb_seqno       HSE_ALIGNED(SMP_CACHE_BYTES * 2);
    struct kvdb_rparams ikdb_rp HSE_ALIGNED(SMP_CACHE_BYTES * 2);
    struct kvdb_ctxn_bkt        ikdb_ctxn_cache[KVDB_CTXN_BKT_MAX];

    /* Put the mostly cold data at end of the structure to improve
     * the density of the hotter data.
     */
    struct mutex       ikdb_lock;
    u32                ikdb_kvs_cnt;
    struct kvdb_kvs *  ikdb_kvs_vec[HSE_KVS_COUNT_MAX];
    struct work_struct ikdb_maint_work;
    struct work_struct ikdb_throttle_work;

    struct mclass_policy ikdb_mpolicies[HSE_MPOLICY_COUNT];

    u64            ikdb_cndb_oid1;
    u64            ikdb_cndb_oid2;
    u64            ikdb_wal_oid1;
    u64            ikdb_wal_oid2;
    char           ikdb_home[PATH_MAX];
    struct pidfh * ikdb_pidfh;
    struct config *ikdb_config;
};

struct ikvdb *
ikvdb_kvdb_handle(struct ikvdb_impl *self)
{
    return &self->ikdb_handle;
}

void
ikvdb_perfc_alloc(struct ikvdb_impl *self)
{
    char   dbname_buf[DT_PATH_COMP_ELEMENT_LEN];
    size_t n;

    dbname_buf[0] = 0;

    n = strlcpy(dbname_buf, self->ikdb_home, sizeof(dbname_buf));
    if (ev(n >= sizeof(dbname_buf)))
        return;

    if (perfc_ctrseti_alloc(
            COMPNAME, dbname_buf, ctxn_perfc_op, PERFC_EN_CTXNOP, "set", &self->ikdb_ctxn_op))
        hse_log(HSE_ERR "cannot alloc ctxn op perf counters");
}

static void
ikvdb_perfc_free(struct ikvdb_impl *self)
{
    perfc_ctrseti_free(&self->ikdb_ctxn_op);
}

merr_t
validate_kvs_name(const char *name)
{
    int name_len;

    if (ev(!name || !*name))
        return merr(EINVAL);

    name_len = strnlen(name, HSE_KVS_NAME_LEN_MAX);

    if (name_len == HSE_KVS_NAME_LEN_MAX)
        return merr(ev(ENAMETOOLONG));

    /* Does the name contain invalid characters ?
     * i.e. char apart from [-_A-Za-z0-9]
     */
    while (*name && name_len-- > 0) {
        if (ev(!isalnum(*name) && *name != '_' && *name != '-'))
            return merr(EINVAL);
        ++name;
    }

    if (ev(*name))
        return merr(ev(ENAMETOOLONG));

    return 0;
}

static merr_t
ikvdb_wal_create(
    struct mpool        *mp,
    struct kvdb_cparams *cp,
    struct kvdb_log     *log,
    struct kvdb_log_tx **tx)
{
    uint64_t mdcid1, mdcid2;
    merr_t err;

    err = wal_create(mp, cp, &mdcid1, &mdcid2);
    if (err)
        return err;

    err = kvdb_log_mdc_create(log, KVDB_LOG_MDC_ID_WAL, mdcid1, mdcid2, tx);
    if (err) {
        wal_destroy(mp, mdcid1, mdcid2);
        return err;
    }

    err = kvdb_log_done(log, *tx);
    if (err)
        goto errout;

    return 0;

errout:
    wal_destroy(mp, mdcid1, mdcid2);
    kvdb_log_abort(log, *tx);

    return err;
}

merr_t
ikvdb_log_deserialize_to_kvdb_rparams(const char *kvdb_home, struct kvdb_rparams *params)
{
    return kvdb_log_deserialize_to_kvdb_rparams(kvdb_home, params);
}

merr_t
ikvdb_log_deserialize_to_kvdb_dparams(const char *kvdb_home, struct kvdb_dparams *params)
{
    return kvdb_log_deserialize_to_kvdb_dparams(kvdb_home, params);
}

merr_t
ikvdb_create(const char *kvdb_home, struct mpool *mp, struct kvdb_cparams *params, u64 captgt)
{
    assert(mp);
    assert(params);

    struct kvdb_log *   log = NULL;
    merr_t              err;
    u64                 cndb_o1, cndb_o2;
    u64                 cndb_captgt;
    struct kvdb_log_tx *tx = NULL;

    cndb_o1 = 0;
    cndb_o2 = 0;

    err = mpool_mdc_root_create(kvdb_home);
    if (err)
        return err;

    err = kvdb_log_open(kvdb_home, mp, O_RDWR, &log);
    if (ev(err))
        goto out;

    err = kvdb_log_create(log, captgt, params);
    if (ev(err))
        goto out;

    cndb_captgt = 0;
    err = cndb_alloc(mp, &cndb_captgt, &cndb_o1, &cndb_o2);
    if (ev(err))
        goto out;

    err = kvdb_log_mdc_create(log, KVDB_LOG_MDC_ID_CNDB, cndb_o1, cndb_o2, &tx);
    if (ev(err))
        goto out;

    err = cndb_create(mp, cndb_captgt, cndb_o1, cndb_o2);
    if (ev(err)) {
        kvdb_log_abort(log, tx);
        goto out;
    }

    err = kvdb_log_done(log, tx);
    if (ev(err))
        goto out;

    err = ikvdb_wal_create(mp, params, log, &tx);

out:
    /* Failed ikvdb_create() indicates that the caller or operator should
     * destroy the kvdb: recovery is not possible.
     */
    kvdb_log_close(log);

    return err;
}

static inline void
ikvdb_tb_configure(struct ikvdb_impl *self, u64 burst, u64 rate, bool initialize)
{
    if (initialize)
        tbkt_init(&self->ikdb_tb, burst, rate);
    else
        tbkt_adjust(&self->ikdb_tb, burst, rate);
}

static void
ikvdb_rate_limit_set(struct ikvdb_impl *self, u64 rate)
{
    u64 burst = rate / 2;

    /* cache debug params from KVDB runtime params */
    self->ikdb_tb_dbg = self->ikdb_rp.throttle_debug & THROTTLE_DEBUG_TB_MASK;

    /* debug: manual control: get burst and rate from params  */
    if (HSE_UNLIKELY(self->ikdb_tb_dbg & THROTTLE_DEBUG_TB_MANUAL)) {
        burst = self->ikdb_rp.throttle_burst;
        rate = self->ikdb_rp.throttle_rate;
    }

    if (burst != self->ikdb_tb_burst || rate != self->ikdb_tb_rate) {
        self->ikdb_tb_burst = burst;
        self->ikdb_tb_rate = rate;
        ikvdb_tb_configure(self, self->ikdb_tb_burst, self->ikdb_tb_rate, false);
    }

    if (self->ikdb_tb_dbg) {

        u64 now = get_time_ns();

        if (now > self->ikdb_tb_dbg_next) {

            /* periodic debug output */
            long dbg_ops = atomic64_read(&self->ikdb_tb_dbg_ops);
            long dbg_bytes = atomic64_read(&self->ikdb_tb_dbg_bytes);
            long dbg_sleep_ns = atomic64_read(&self->ikdb_tb_dbg_sleep_ns);

            hse_log(
                HSE_NOTICE " tbkt_debug: manual %d shunt %d ops %8ld  bytes %10ld"
                           " sleep_ns %12ld burst %10lu rate %10lu raw %10lu",
                (bool)(self->ikdb_tb_dbg & THROTTLE_DEBUG_TB_MANUAL),
                (bool)(self->ikdb_tb_dbg & THROTTLE_DEBUG_TB_SHUNT),
                dbg_ops,
                dbg_bytes,
                dbg_sleep_ns,
                self->ikdb_tb_burst,
                self->ikdb_tb_rate,
                throttle_delay(&self->ikdb_throttle));

            atomic64_sub(dbg_ops, &self->ikdb_tb_dbg_ops);
            atomic64_sub(dbg_bytes, &self->ikdb_tb_dbg_bytes);
            atomic64_sub(dbg_sleep_ns, &self->ikdb_tb_dbg_sleep_ns);

            self->ikdb_tb_dbg_next = now + NSEC_PER_SEC;
        }
    }
}

static void
ikvdb_throttle_task(struct work_struct *work)
{
    struct ikvdb_impl *self;
    u64                throttle_update_prev = 0;

    self = container_of(work, struct ikvdb_impl, ikdb_throttle_work);

    while (!self->ikdb_work_stop) {

        u64 tstart = get_time_ns();

        if (tstart > throttle_update_prev + self->ikdb_rp.throttle_update_ns) {

            uint raw = throttle_update(&self->ikdb_throttle);
            u64  rate = throttle_raw_to_rate(raw);

            ikvdb_rate_limit_set(self, rate);
            throttle_update_prev = tstart;
        }

        /* Sleep for 10ms minus processing overhead.  Does not account
         * for sleep time variance, but does account for timer slack
         * to minimize drift.
         */
        tstart = (get_time_ns() - tstart + timer_slack) / 1000;
        if (tstart < 10000)
            usleep(10000 - tstart);
    }
}

static void
ikvdb_maint_task(struct work_struct *work)
{
    struct ikvdb_impl *self;
    u64                curcnt_warn = 0;
    u64                maxdelay;

    self = container_of(work, struct ikvdb_impl, ikdb_maint_work);

    maxdelay = 10000; /* 10ms initial delay time */

    while (!self->ikdb_work_stop) {
        uint64_t vadd = 0, vsub = 0, curcnt;
        u64      tstart = get_time_ns();
        uint     i;

        /* Lazily sample the active cursor count and update ikdb_curcnt if necessary.
         * ikvdb_kvs_cursor_create() checks ikdb_curcnt to prevent the creation
         * of an excessive number of cursors.
         */
        perfc_read(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_CURCNT, &vadd, &vsub);

        curcnt = (vadd > vsub) ? (vadd - vsub) : 0;

        if (atomic_read(&self->ikdb_curcnt) != curcnt) {
            atomic_set(&self->ikdb_curcnt, curcnt);

            if (ev(curcnt > self->ikdb_curcnt_max && tstart > curcnt_warn)) {
                hse_log(
                    HSE_WARNING "%s: active cursors (%lu) > max allowed (%u)",
                    __func__,
                    curcnt,
                    self->ikdb_curcnt_max);

                curcnt_warn = tstart + NSEC_PER_SEC * 15;
            }
        }

        /* [HSE_REVISIT] move from big lock to using refcnts for
         * accessing KVSes in the kvs vector. Here and in all admin
         * functions
         */
        mutex_lock(&self->ikdb_lock);
        for (i = 0; i < self->ikdb_kvs_cnt; i++) {
            struct kvdb_kvs *kvs = self->ikdb_kvs_vec[i];

            if (kvs && kvs->kk_ikvs)
                kvs_maint_task(kvs->kk_ikvs, tstart);
        }
        mutex_unlock(&self->ikdb_lock);

        /* Sleep for 100ms minus processing overhead.  Does not account
         * for sleep time variance.  Divide delta by 1024 rather than
         * 1000 to facilitate intentional drift.
         */
        tstart = (get_time_ns() - tstart) / 1024;
        if (tstart < maxdelay)
            usleep(maxdelay - tstart);

        /* Use a smaller delay at program start to avoid unnecessarily
         * holding up a short lived program.  Once we hit 100ms we'll
         * stop incrementing maxdelay.
         */
        if (maxdelay < 100000)
            maxdelay += 3000;
    }
}

static void
ikvdb_init_throttle_params(struct ikvdb_impl *self)
{
    if (self->ikdb_rdonly)
        return;

    /* Hand out throttle sensors */
    csched_throttle_sensor(
        self->ikdb_csched, throttle_sensor(&self->ikdb_throttle, THROTTLE_SENSOR_CSCHED));

    c0sk_throttle_sensor(
        self->ikdb_c0sk, throttle_sensor(&self->ikdb_throttle, THROTTLE_SENSOR_C0SK));
}

static void
ikvdb_txn_init(struct ikvdb_impl *self)
{
    int i;

    for (i = 0; i < NELEM(self->ikdb_ctxn_cache); ++i) {
        struct kvdb_ctxn_bkt *bkt = self->ikdb_ctxn_cache + i;

        spin_lock_init(&bkt->kcb_lock);
    }
}

static void
ikvdb_txn_fini(struct ikvdb_impl *self)
{
    int i, j;

    for (i = 0; i < NELEM(self->ikdb_ctxn_cache); ++i) {
        struct kvdb_ctxn_bkt *bkt = self->ikdb_ctxn_cache + i;

        for (j = 0; j < bkt->kcb_ctxnc; ++j)
            kvdb_ctxn_free(bkt->kcb_ctxnv[j]);
    }
}

merr_t
ikvdb_diag_cndb(struct ikvdb *handle, struct cndb **cndb)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    if (!self || !cndb)
        return merr(ev(EINVAL));

    *cndb = self->ikdb_cndb;

    return 0;
}

/* exposes kvs details to, e.g., kvck */
merr_t
ikvdb_diag_kvslist(struct ikvdb *handle, struct diag_kvdb_kvs_list *list, int len, int *kvscnt)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    int    i, c;
    merr_t err;

    if (!handle || !list || !kvscnt)
        return merr(ev(EINVAL));

    err = cndb_cn_count(self->ikdb_cndb, &self->ikdb_kvs_cnt);
    if (ev(err))
        return err;

    c = (len < self->ikdb_kvs_cnt) ? len : self->ikdb_kvs_cnt;

    *kvscnt = self->ikdb_kvs_cnt;

    for (i = 0; i < c; i++) {
        u64 cnid = 0;

        err = cndb_cn_info_idx(
            self->ikdb_cndb, i, &cnid, NULL, NULL, list[i].kdl_name, sizeof(list[i].kdl_name));
        if (ev(err))
            break;

        list[i].kdl_cnid = cnid;
    }

    return err;
}

/* ikvdb_diag_open() - open relevant media streams with minimal processing. */
merr_t
ikvdb_diag_open(
    const char *         kvdb_home,
    struct pidfh *       pfh,
    struct mpool *       mp,
    struct kvdb_rparams *params,
    struct ikvdb **      handle)
{
    struct ikvdb_impl *self;
    size_t             n;
    merr_t             err;
    char               cwd[sizeof(self->ikdb_home)];

    if (!kvdb_home) {
        if (!getcwd(cwd, sizeof(cwd)))
            return merr(errno);
        kvdb_home = cwd;
    } else {
        kvdb_home = kvdb_home;
    }

    /* [HSE_REVISIT] consider factoring out this code into ikvdb_cmn_open
     * and calling that from here and ikvdb_open.
     */
    self = aligned_alloc(alignof(*self), sizeof(*self));
    if (ev(!self))
        return merr(ENOMEM);

    memset(self, 0, sizeof(*self));

    self->ikdb_pidfh = pfh;

    n = strlcpy(self->ikdb_home, kvdb_home, sizeof(self->ikdb_home));
    if (ev(n >= sizeof(self->ikdb_home))) {
        err = merr(ENAMETOOLONG);
        goto err_exit0;
    }

    self->ikdb_mp = mp;

    assert(params);
    self->ikdb_rp = *params;
    self->ikdb_rdonly = params->read_only;
    params = &self->ikdb_rp;

    atomic_set(&self->ikdb_curcnt, 0);

    ikvdb_txn_init(self);

    err = viewset_create(&self->ikdb_txn_viewset, &self->ikdb_seqno);
    if (ev(err))
        goto err_exit0;

    err = viewset_create(&self->ikdb_cur_viewset, &self->ikdb_seqno);
    if (ev(err))
        goto err_exit1;

    err = kvdb_keylock_create(&self->ikdb_keylock, params->keylock_tables);
    if (ev(err))
        goto err_exit1;

    err = kvdb_log_open(kvdb_home, mp, params->read_only ? O_RDONLY : O_RDWR, &self->ikdb_log);
    if (ev(err))
        goto err_exit2;

    err = kvdb_log_replay(self->ikdb_log);
    if (ev(err))
        goto err_exit3;

    kvdb_log_cndboid_get(self->ikdb_log, &self->ikdb_cndb_oid1, &self->ikdb_cndb_oid2);

    err = cndb_open(
        mp,
        self->ikdb_rdonly,
        &self->ikdb_seqno,
        params->cndb_entries,
        self->ikdb_cndb_oid1,
        self->ikdb_cndb_oid2,
        &self->ikdb_health,
        &self->ikdb_rp,
        &self->ikdb_cndb);

    if (!ev(err)) {
        *handle = &self->ikdb_handle;
        return 0;
    }

err_exit3:
    kvdb_log_close(self->ikdb_log);

err_exit2:
    kvdb_keylock_destroy(self->ikdb_keylock);

err_exit1:
    viewset_destroy(self->ikdb_cur_viewset);
    viewset_destroy(self->ikdb_txn_viewset);

err_exit0:
    ikvdb_txn_fini(self);
    free(self);
    *handle = NULL;

    return err;
}

merr_t
ikvdb_diag_close(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    merr_t             err;
    merr_t             ret = 0; /* store the first error encountered */

    self->ikdb_work_stop = true;
    mutex_lock(&self->ikdb_lock);

    err = cndb_close(self->ikdb_cndb);
    if (ev(err))
        ret = ret ?: err;

    err = kvdb_log_close(self->ikdb_log);
    if (ev(err))
        ret = ret ?: err;

    mutex_unlock(&self->ikdb_lock);

    ikvdb_txn_fini(self);

    viewset_destroy(self->ikdb_cur_viewset);
    viewset_destroy(self->ikdb_txn_viewset);

    kvdb_keylock_destroy(self->ikdb_keylock);
    mutex_destroy(&self->ikdb_lock);

    free(self);

    return ret;
}

/**
 * ikvdb_rest_register() - install rest handlers for KVSes and the kvs list
 * @self:       self
 * @handle:     ikvdb handle
 */
static void
ikvdb_rest_register(struct ikvdb_impl *self, struct ikvdb *handle)
{
    int    i;
    merr_t err;

    for (i = 0; i < self->ikdb_kvs_cnt; i++) {
        err = kvs_rest_register(self->ikdb_kvs_vec[i]->kk_name, self->ikdb_kvs_vec[i]);
        if (err)
            hse_elog(
                HSE_WARNING "%s/%s REST registration failed: @@e",
                err,
                self->ikdb_home,
                self->ikdb_kvs_vec[i]->kk_name);
    }

    err = kvdb_rest_register(handle);
    if (err)
        hse_elog(HSE_WARNING "%s REST registration failed: @@e", err, self->ikdb_home);
}

/**
 * ikvdb_maint_start() - start maintenance work queue
 * @self:       self
 */
static merr_t
ikvdb_maint_start(struct ikvdb_impl *self)
{
    merr_t err;

    self->ikdb_work_stop = false;
    self->ikdb_workqueue = alloc_workqueue("kvdb_maint", 0, 3);
    if (!self->ikdb_workqueue) {
        err = merr(ENOMEM);
        hse_elog(HSE_ERR "%s cannot start kvdb maintenance", err, self->ikdb_home);
        return err;
    }

    INIT_WORK(&self->ikdb_maint_work, ikvdb_maint_task);
    if (!queue_work(self->ikdb_workqueue, &self->ikdb_maint_work)) {
        err = merr(EBUG);
        hse_elog(HSE_ERR "%s cannot start kvdb maintenance", err, self->ikdb_home);
        return err;
    }

    INIT_WORK(&self->ikdb_throttle_work, ikvdb_throttle_task);
    if (!queue_work(self->ikdb_workqueue, &self->ikdb_throttle_work)) {
        err = merr(EBUG);
        hse_elog(HSE_ERR "%s cannot start kvdb throttle", err, self->ikdb_home);
        return err;
    }

    return 0;
}

static struct kvdb_kvs *
kvdb_kvs_create(void)
{
    struct kvdb_kvs *kvs;

    kvs = aligned_alloc(alignof(*kvs), sizeof(*kvs));
    if (kvs) {
        memset(kvs, 0, sizeof(*kvs));
        kvs->kk_vcompmin = UINT_MAX;
        atomic_set(&kvs->kk_refcnt, 0);
    }

    return kvs;
}

static void
kvdb_kvs_destroy(struct kvdb_kvs *kvs)
{
    if (kvs) {
        assert(atomic_read(&kvs->kk_refcnt) == 0);
        memset(kvs, -1, sizeof(*kvs));
        free(kvs);
    }
}

/**
 * ikvdb_cndb_open() - instantiate multi-kvs metadata
 * @self:       self
 * @seqno:      sequence number (output)
 * @ingestid:   ingest id (output)
 */
static merr_t
ikvdb_cndb_open(struct ikvdb_impl *self, u64 *seqno, u64 *ingestid)
{
    merr_t           err = 0;
    int              i;
    struct kvdb_kvs *kvs;

    err = cndb_open(
        self->ikdb_mp,
        self->ikdb_rdonly,
        &self->ikdb_seqno,
        self->ikdb_rp.cndb_entries,
        self->ikdb_cndb_oid1,
        self->ikdb_cndb_oid2,
        &self->ikdb_health,
        &self->ikdb_rp,
        &self->ikdb_cndb);
    if (ev(err))
        goto err_exit;

    err = cndb_replay(self->ikdb_cndb, seqno, ingestid);
    if (ev(err))
        goto err_exit;

    err = cndb_cn_count(self->ikdb_cndb, &self->ikdb_kvs_cnt);
    if (ev(err))
        goto err_exit;

    for (i = 0; i < self->ikdb_kvs_cnt; i++) {
        kvs = kvdb_kvs_create();
        if (ev(!kvs)) {
            err = merr(ENOMEM);
            goto err_exit;
        }

        self->ikdb_kvs_vec[i] = kvs;

        err = cndb_cn_info_idx(
            self->ikdb_cndb,
            i,
            &kvs->kk_cnid,
            &kvs->kk_flags,
            &kvs->kk_cparams,
            kvs->kk_name,
            sizeof(kvs->kk_name));
        if (ev(err))
            goto err_exit;
    }

err_exit:
    return err;
}

/**
 * ikvdb_low_mem_adjust() - configure for constrained memory environment
 * @self:       self
 */
static void
ikvdb_low_mem_adjust(struct ikvdb_impl *self)
{
    struct kvdb_rparams  dflt = kvdb_rparams_defaults();
    struct kvdb_rparams *kp = &self->ikdb_rp;

    ulong mavail;
    uint  scale;

    hse_log(HSE_WARNING "configuring %s for constrained memory environment", self->ikdb_home);

    /* The default parameter values in this function enables us to run
     * in a memory constrained cgroup. Scale the parameter values based
     * on the available memory. This function is called only when the
     * total RAM is <= 32G. Based on some experiments, the scale factor
     * is set to 8G.
     */
    hse_meminfo(NULL, &mavail, 30);
    scale = mavail / 8;
    scale = max_t(uint, 1, scale);

    if (kp->c0_cheap_cache_sz_max == dflt.c0_cheap_cache_sz_max)
        kp->c0_cheap_cache_sz_max = min_t(u64, 1024 * 1024 * 128UL * scale, HSE_C0_CCACHE_SZ_MAX);

    if (kp->c0_cheap_sz == dflt.c0_cheap_sz)
        kp->c0_cheap_sz = min_t(u64, HSE_C0_CHEAP_SZ_MIN * scale, HSE_C0_CHEAP_SZ_MAX);

    if (kp->c0_ingest_width == dflt.c0_ingest_width)
        kp->c0_ingest_width = HSE_C0_INGEST_WIDTH_MIN;

    if (kp->c0_ingest_threads == dflt.c0_ingest_threads)
        kp->c0_ingest_threads = min_t(u64, scale, HSE_C0_INGEST_THREADS_DFLT);

    if (kp->c0_mutex_pool_sz == dflt.c0_mutex_pool_sz)
        kp->c0_mutex_pool_sz = 5;

    if (kp->throttle_c0_hi_th == dflt.throttle_c0_hi_th)
        kp->throttle_c0_hi_th = (2 * kp->c0_cheap_sz * kp->c0_ingest_width) >> 20;

    c0kvs_reinit(kp->c0_cheap_cache_sz_max);
}

static void
ikvdb_wal_cningest_cb(
    struct ikvdb *ikdb,
    unsigned long seqno,
    unsigned long gen,
    unsigned long txhorizon,
    bool          post_ingest)
{
    struct ikvdb_impl *self = ikvdb_h2r(ikdb);

    if (self->ikdb_wal)
        wal_cningest_cb(self->ikdb_wal, seqno, gen, txhorizon, post_ingest);
}

static void
ikvdb_wal_install_callback(struct ikvdb_impl *self)
{
    struct kvdb_callback *cb = &self->ikdb_wal_cb;

    if (!self->ikdb_wal) {
        c0sk_install_callback(self->ikdb_c0sk, NULL);
        return;
    }

    cb->kc_cbarg = &self->ikdb_handle;
    cb->kc_cningest_cb = ikvdb_wal_cningest_cb;

    c0sk_install_callback(self->ikdb_c0sk, cb);
}

merr_t
ikvdb_open(
    const char *               kvdb_home,
    const struct kvdb_rparams *params,
    struct pidfh *             pfh,
    struct mpool *             mp,
    struct config *            conf,
    struct ikvdb **            handle)
{
    merr_t             err;
    struct ikvdb_impl *self;
    u64                seqno = 0; /* required by unit test */
    ulong              mavail;
    size_t             sz, n;
    int                i;
    u64                ingestid, gen = 0;

    assert(kvdb_home);
    assert(params);

    self = aligned_alloc(alignof(*self), sizeof(*self));
    if (ev(!self)) {
        err = merr(ENOMEM);
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        return err;
    }

    memset(self, 0, sizeof(*self));
    mutex_init(&self->ikdb_lock);
    ikvdb_txn_init(self);
    self->ikdb_mp = mp;

    self->ikdb_pidfh = pfh;

    n = strlcpy(self->ikdb_home, kvdb_home, sizeof(self->ikdb_home));
    if (n >= sizeof(self->ikdb_home)) {
        err = merr(ENAMETOOLONG);
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err2;
    }

    memcpy(self->ikdb_mpolicies, params->mclass_policies, sizeof(params->mclass_policies));

    self->ikdb_rp = *params;
    self->ikdb_rdonly = params->read_only;

    hse_meminfo(NULL, &mavail, 0);
    if (params->low_mem || (mavail >> 30) < 32)
        ikvdb_low_mem_adjust(self);

    throttle_init(&self->ikdb_throttle, &self->ikdb_rp);
    throttle_init_params(&self->ikdb_throttle, &self->ikdb_rp);

    self->ikdb_tb_burst = self->ikdb_rp.throttle_burst;
    self->ikdb_tb_rate = self->ikdb_rp.throttle_rate;

    ikvdb_tb_configure(self, self->ikdb_tb_burst, self->ikdb_tb_rate, true);

    if (!self->ikdb_rdonly) {
        err = csched_create(
            csched_rp_policy(&self->ikdb_rp),
            self->ikdb_mp,
            &self->ikdb_rp,
            self->ikdb_home,
            &self->ikdb_health,
            &self->ikdb_csched);
        if (err) {
            hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
            goto err1;
        }
    }

    /* Set max number of active cursors per kvdb such that max
     * memory use is limited to about 10% of system memory.
     */
    sz = (mavail * HSE_CURACTIVE_SZ_PCT) / 100;
    sz = clamp_t(size_t, sz, HSE_CURACTIVE_SZ_MIN, HSE_CURACTIVE_SZ_MAX);
    self->ikdb_curcnt_max = sz / HSE_CURSOR_SZ_MIN;

    atomic_set(&self->ikdb_curcnt, 0);
    atomic64_set(&self->ikdb_seqno, 1);

    err = viewset_create(&self->ikdb_txn_viewset, &self->ikdb_seqno);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = viewset_create(&self->ikdb_cur_viewset, &self->ikdb_seqno);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = kvdb_keylock_create(&self->ikdb_keylock, params->keylock_tables);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = kvdb_log_open(kvdb_home, mp, self->ikdb_rdonly ? O_RDONLY : O_RDWR, &self->ikdb_log);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = kvdb_log_replay(self->ikdb_log);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    kvdb_log_cndboid_get(self->ikdb_log, &self->ikdb_cndb_oid1, &self->ikdb_cndb_oid2);
    err = ikvdb_cndb_open(self, &seqno, &ingestid);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    atomic64_set(&self->ikdb_seqno, seqno);

    err = kvdb_ctxn_set_create(
        &self->ikdb_ctxn_set, self->ikdb_rp.txn_timeout, self->ikdb_rp.txn_wkth_delay);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = c0snr_set_create(kvdb_ctxn_abort, &self->ikdb_c0snr_set);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = cn_kvdb_create(&self->ikdb_cn_kvdb);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    err = lc_create(&self->ikdb_lc, &self->ikdb_health);
    if (ev(err)) {
        hse_elog(HSE_ERR "failed to create lc: @@e", err);
        goto err1;
    }

    lc_ingest_seqno_set(self->ikdb_lc, atomic64_read(&self->ikdb_seqno));

    if (ingestid != CNDB_INVAL_INGESTID && ingestid != CNDB_DFLT_INGESTID && ingestid > 0)
        gen = ingestid;

    err = c0sk_open(
        &self->ikdb_rp,
        mp,
        self->ikdb_home,
        &self->ikdb_health,
        self->ikdb_csched,
        &self->ikdb_seqno,
        gen,
        &self->ikdb_c0sk);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    c0sk_lc_set(self->ikdb_c0sk, self->ikdb_lc);
    c0sk_ctxn_set_set(self->ikdb_c0sk, self->ikdb_ctxn_set);

    kvdb_log_waloid_get(self->ikdb_log, &self->ikdb_wal_oid1, &self->ikdb_wal_oid2);
    err = wal_open(mp, &self->ikdb_rp, self->ikdb_wal_oid1, self->ikdb_wal_oid2,
                   &self->ikdb_health, &self->ikdb_wal);
    if (err) {
        hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
        goto err1;
    }

    *handle = &self->ikdb_handle;

    if (!self->ikdb_rdonly) {
        err = ikvdb_maint_start(self);
        if (err) {
            hse_elog(HSE_ERR "cannot open %s: @@e", err, kvdb_home);
            goto err1;
        }
    }

    ikvdb_wal_install_callback(self);

    ikvdb_perfc_alloc(self);
    kvdb_keylock_perfc_init(self->ikdb_keylock, &self->ikdb_ctxn_op);

    ikvdb_rest_register(self, &self->ikdb_handle);

    ikvdb_init_throttle_params(self);

    self->ikdb_config = conf;

    *handle = &self->ikdb_handle;

    return 0;

err1:
    c0sk_close(self->ikdb_c0sk);
    lc_destroy(self->ikdb_lc);
    self->ikdb_work_stop = true;
    destroy_workqueue(self->ikdb_workqueue);
    cn_kvdb_destroy(self->ikdb_cn_kvdb);
    for (i = 0; i < self->ikdb_kvs_cnt; i++)
        kvdb_kvs_destroy(self->ikdb_kvs_vec[i]);
    c0snr_set_destroy(self->ikdb_c0snr_set);
    kvdb_ctxn_set_destroy(self->ikdb_ctxn_set);
    wal_close(self->ikdb_wal);
    cndb_close(self->ikdb_cndb);
    kvdb_log_close(self->ikdb_log);
    kvdb_keylock_destroy(self->ikdb_keylock);
    viewset_destroy(self->ikdb_cur_viewset);
    viewset_destroy(self->ikdb_txn_viewset);
    csched_destroy(self->ikdb_csched);
    throttle_fini(&self->ikdb_throttle);

err2:
    ikvdb_txn_fini(self);
    mutex_destroy(&self->ikdb_lock);
    free(self);
    *handle = NULL;

    return err;
}

struct pidfh *
ikvdb_pidfh(struct ikvdb *kvdb)
{
    struct ikvdb_impl *self = ikvdb_h2r(kvdb);

    return self->ikdb_pidfh;
}

const char *
ikvdb_home(struct ikvdb *kvdb)
{
    struct ikvdb_impl *self = ikvdb_h2r(kvdb);

    return self->ikdb_home;
}

struct config *
ikvdb_config(struct ikvdb *kvdb)
{
    struct ikvdb_impl *self = ikvdb_h2r(kvdb);

    return self->ikdb_config;
}

bool
ikvdb_rdonly(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    return self->ikdb_rdonly;
}

void
ikvdb_get_c0sk(struct ikvdb *handle, struct c0sk **out)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    *out = self->ikdb_c0sk;
}

struct csched *
ikvdb_get_csched(struct ikvdb *handle)
{
    return handle ? ikvdb_h2r(handle)->ikdb_csched : 0;
}

struct mclass_policy *
ikvdb_get_mclass_policy(struct ikvdb *handle, const char *name)
{
    struct ikvdb_impl *   self = ikvdb_h2r(handle);
    struct mclass_policy *policy = self->ikdb_mpolicies;
    int                   i;

    for (i = 0; i < HSE_MPOLICY_COUNT; i++, policy++)
        if (!strcmp(policy->mc_name, name))
            return policy;

    return NULL;
}

static int
get_kvs_index(struct kvdb_kvs **list, const char *kvs_name, int *avail)
{
    int i, av = -1;

    for (i = 0; i < HSE_KVS_COUNT_MAX; i++) {
        if (!list[i])
            av = av < 0 ? i : av;
        else if (!strcmp(kvs_name, list[i]->kk_name))
            return i;
    }

    if (avail)
        *avail = av;

    return -1;
}

static void
drop_kvs_index(struct ikvdb *handle, int idx)
{
    int                c;
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    c = self->ikdb_kvs_cnt - idx - 1;
    kvdb_kvs_destroy(self->ikdb_kvs_vec[idx]);
    if (c)
        memmove(
            &self->ikdb_kvs_vec[idx], &self->ikdb_kvs_vec[idx + 1], c * sizeof(struct kvdb_kvs *));
    self->ikdb_kvs_vec[--self->ikdb_kvs_cnt] = NULL;
}

struct ikvdb_impl *
kvdb_kvs_parent(struct kvdb_kvs *kk)
{
    return kk->kk_parent;
}

struct kvs_cparams *
kvdb_kvs_cparams(struct kvdb_kvs *kk)
{
    return kk->kk_cparams;
}

u32
kvdb_kvs_flags(struct kvdb_kvs *kk)
{
    return kk->kk_flags;
}

u64
kvdb_kvs_cnid(struct kvdb_kvs *kk)
{
    return kk->kk_cnid;
}

char *
kvdb_kvs_name(struct kvdb_kvs *kk)
{
    return kk->kk_name;
}

void
kvdb_kvs_set_ikvs(struct kvdb_kvs *kk, struct ikvs *ikvs)
{
    kk->kk_ikvs = ikvs;
}

merr_t
ikvdb_kvs_create(struct ikvdb *handle, const char *kvs_name, const struct kvs_cparams *params)
{
    assert(handle);
    assert(kvs_name);
    assert(params);

    merr_t             err = 0;
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct kvdb_kvs *  kvs = NULL;
    int                idx;

    if (self->ikdb_rdonly)
        goto out_immediate;

    err = validate_kvs_name(kvs_name);
    if (ev(err))
        goto out_immediate;

    kvs = kvdb_kvs_create();
    if (ev(!kvs)) {
        err = merr(ENOMEM);
        goto out_immediate;
    }

    strlcpy(kvs->kk_name, kvs_name, sizeof(kvs->kk_name));

    mutex_lock(&self->ikdb_lock);

    if (self->ikdb_kvs_cnt >= HSE_KVS_COUNT_MAX) {
        err = merr(ev(EINVAL));
        goto out_unlock;
    }

    if (get_kvs_index(self->ikdb_kvs_vec, kvs_name, &idx) >= 0) {
        err = merr(ev(EEXIST));
        goto out_unlock;
    }

    assert(idx >= 0); /* assert we found an empty slot */

    kvs->kk_flags = cn_cp2cflags(params);

    err = cndb_cn_create(self->ikdb_cndb, params, &kvs->kk_cnid, kvs->kk_name);
    if (ev(err))
        goto out_unlock;

    kvs->kk_cparams = cndb_cn_cparams(self->ikdb_cndb, kvs->kk_cnid);

    if (ev(!kvs->kk_cparams)) {
        cndb_cn_drop(self->ikdb_cndb, kvs->kk_cnid);
        err = merr(EBUG);
        goto out_unlock;
    }

    assert(kvs->kk_cparams);

    self->ikdb_kvs_cnt++;
    self->ikdb_kvs_vec[idx] = kvs;

    mutex_unlock(&self->ikdb_lock);

    /* Register in kvs make instead of open so all KVSes can be queried for
     * info
     */
    err = kvs_rest_register(self->ikdb_kvs_vec[idx]->kk_name, self->ikdb_kvs_vec[idx]);
    if (ev(err))
        hse_elog(
            HSE_WARNING "rest: %s registration failed: @@e", err, self->ikdb_kvs_vec[idx]->kk_name);

    return 0;

out_unlock:
    mutex_unlock(&self->ikdb_lock);
out_immediate:
    if (err && kvs)
        kvdb_kvs_destroy(kvs);

    return err;
}

merr_t
ikvdb_kvs_drop(struct ikvdb *handle, const char *kvs_name)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct kvdb_kvs *  kvs;
    int                idx;
    merr_t             err;

    if (self->ikdb_rp.read_only) {
        err = merr(ev(EROFS));
        goto out_immediate;
    }

    err = validate_kvs_name(kvs_name);
    if (ev(err))
        goto out_immediate;

    mutex_lock(&self->ikdb_lock);

    idx = get_kvs_index(self->ikdb_kvs_vec, kvs_name, NULL);
    if (idx < 0) {
        err = merr(ev(ENOENT));
        goto out_unlock;
    }

    kvs = self->ikdb_kvs_vec[idx];

    if (kvs->kk_ikvs) {
        err = merr(ev(EBUSY));
        goto out_unlock;
    }

    kvs_rest_deregister(kvs->kk_name);

    /* kvs_rest_deregister() waits until all active rest requests
     * have finished. Verify that the refcnt has gone down to zero
     */
    assert(atomic_read(&kvs->kk_refcnt) == 0);

    err = cndb_cn_drop(self->ikdb_cndb, kvs->kk_cnid);
    if (ev(err))
        goto out_unlock;

    drop_kvs_index(handle, idx);

out_unlock:
    mutex_unlock(&self->ikdb_lock);
out_immediate:
    return err;
}

merr_t
ikvdb_kvs_names_get(struct ikvdb *handle, size_t *namec, char ***namev)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    int                i, slot = 0;
    char **            kvsv;
    char *             name;

    kvsv = calloc(HSE_KVS_COUNT_MAX, sizeof(*kvsv) + HSE_KVS_NAME_LEN_MAX);
    if (!namev)
        return merr(ev(ENOMEM));

    mutex_lock(&self->ikdb_lock);

    /* seek to start of the section holding the strings */
    name = (char *)&kvsv[self->ikdb_kvs_cnt];

    for (i = 0; i < HSE_KVS_COUNT_MAX; i++) {
        struct kvdb_kvs *kvs = self->ikdb_kvs_vec[i];

        if (!kvs)
            continue;

        strlcpy(name, kvs->kk_name, HSE_KVS_NAME_LEN_MAX);

        kvsv[slot++] = name;
        name += HSE_KVS_NAME_LEN_MAX;
    }

    *namev = kvsv;

    if (namec)
        *namec = self->ikdb_kvs_cnt;

    mutex_unlock(&self->ikdb_lock);
    return 0;
}

void
ikvdb_kvs_names_free(struct ikvdb *handle, char **namev)
{
    assert(namev);

    free(namev);
}

void
ikvdb_kvs_count(struct ikvdb *handle, unsigned int *count)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    *count = self->ikdb_kvs_cnt;
}

merr_t
ikvdb_kvs_query_tree(struct hse_kvs *kvs, struct yaml_context *yc, int fd, bool list)
{
    return kvs_rest_query_tree((struct kvdb_kvs *)kvs, yc, fd, list);
}

merr_t
ikvdb_kvs_open(
    struct ikvdb *      handle,
    const char *        kvs_name,
    struct kvs_rparams *params,
    uint                flags,
    struct hse_kvs **   kvs_out)
{
    assert(handle);
    assert(kvs_name);
    assert(params);
    assert(kvs_out);

    const struct compress_ops *cops;
    struct ikvdb_impl *        self = ikvdb_h2r(handle);
    struct kvdb_kvs *          kvs = NULL;
    int                        idx;
    merr_t                     err;

    err = config_deserialize_to_kvs_rparams(self->ikdb_config, kvs_name, params);
    if (ev(err))
        goto out_immediate;

    params->rdonly = self->ikdb_rp.read_only; /* inherit from kvdb */

    ikvdb_wal_install_callback(self); /* TODO: can this be removed? */

    mutex_lock(&self->ikdb_lock);

    idx = get_kvs_index(self->ikdb_kvs_vec, kvs_name, NULL);
    if (idx < 0) {
        err = merr(ev(ENOENT));
        goto out_unlock;
    }

    kvs = self->ikdb_kvs_vec[idx];

    if (kvs->kk_ikvs) {
        err = merr(ev(EBUSY));
        goto out_unlock;
    }

    kvs->kk_parent = self;
    kvs->kk_seqno = &self->ikdb_seqno;
    kvs->kk_viewset = self->ikdb_cur_viewset;

    kvs->kk_vcompmin = UINT_MAX;
    cops = vcomp_compress_ops(params);
    if (cops) {
        assert(cops->cop_compress && cops->cop_estimate);

        kvs->kk_vcompress = cops->cop_compress;
        kvs->kk_vcompmin = max_t(uint, CN_SMALL_VALUE_THRESHOLD, params->vcompmin);

        kvs->kk_vcompbnd = cops->cop_estimate(NULL, tls_vbufsz);
        kvs->kk_vcompbnd = tls_vbufsz - (kvs->kk_vcompbnd - tls_vbufsz);
        assert(kvs->kk_vcompbnd < tls_vbufsz);

        assert(cops->cop_estimate(NULL, HSE_KVS_VALUE_LEN_MAX) < HSE_KVS_VALUE_LEN_MAX + PAGE_SIZE * 2);
    }

    /* Need a lock to prevent ikvdb_close from freeing up resources from
     * under us
     */

    err = kvs_open(
        handle,
        kvs,
        self->ikdb_home,
        self->ikdb_mp,
        self->ikdb_cndb,
        self->ikdb_lc,
        self->ikdb_wal,
        params,
        &self->ikdb_health,
        self->ikdb_cn_kvdb,
        flags);
    if (ev(err))
        goto out_unlock;

    atomic_inc(&kvs->kk_refcnt);

    *kvs_out = (struct hse_kvs *)kvs;

out_unlock:
    mutex_unlock(&self->ikdb_lock);
out_immediate:

    return err;
}

merr_t
ikvdb_kvs_close(struct hse_kvs *handle)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *parent = kk->kk_parent;
    merr_t             err;
    struct ikvs *      ikvs;

    mutex_lock(&parent->ikdb_lock);
    ikvs = kk->kk_ikvs;
    if (ikvs) {
        kk->kk_vcompmin = UINT_MAX;
        kk->kk_ikvs = NULL;
    }
    mutex_unlock(&parent->ikdb_lock);

    if (ev(!ikvs))
        return merr(EBADF);

    /* if refcnt goes down to 1, it would mean we have the only ref.
     * Set it to 0 and proceed
     * if not, keep spinning
     */
    while (atomic_cmpxchg(&kk->kk_refcnt, 1, 0) > 1)
        usleep(333);

    err = kvs_close(ikvs);

    return err;
}

merr_t
ikvdb_storage_info_get(
    struct ikvdb *                handle,
    struct hse_kvdb_storage_info *info,
    char *                        cappath,
    char *                        stgpath,
    size_t                        pathlen)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct mpool *     mp;
    struct mpool_stats stats = {};
    merr_t             err;
    uint64_t           allocated, used;

    mp = ikvdb_mpool_get(handle);
    err = mpool_stats_get(mp, &stats);
    if (ev(err))
        return err;

    info->total_bytes = stats.mps_total;
    info->available_bytes = stats.mps_available;

    info->allocated_bytes = stats.mps_allocated;
    info->used_bytes = stats.mps_used;

    /* Get allocated and used space for kvdb metadata */
    err = kvdb_log_usage(self->ikdb_log, &allocated, &used);
    if (ev(err))
        return err;
    info->allocated_bytes += allocated;
    info->used_bytes += used;

    err = cndb_usage(self->ikdb_cndb, &allocated, &used);
    if (ev(err))
        return err;
    info->allocated_bytes += allocated;
    info->used_bytes += used;

    if (cappath)
        strlcpy(cappath, stats.mps_path[MP_MED_CAPACITY], pathlen);
    if (stgpath)
        strlcpy(stgpath, stats.mps_path[MP_MED_STAGING], pathlen);

    return 0;
}

/* PRIVATE */
struct cn *
ikvdb_kvs_get_cn(struct hse_kvs *kvs)
{
    struct kvdb_kvs *kk = (struct kvdb_kvs *)kvs;

    return kvs_cn(kk->kk_ikvs);
}

struct mpool *
ikvdb_mpool_get(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    return handle ? self->ikdb_mp : NULL;
}

merr_t
ikvdb_close(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    unsigned int       i;
    merr_t             err;
    merr_t             ret = 0; /* store the first error encountered */

    /* Shutdown workqueue
     */
    if (!self->ikdb_rdonly) {
        self->ikdb_work_stop = true;
        destroy_workqueue(self->ikdb_workqueue);
    }

    /* Deregistering this url before trying to get ikdb_lock prevents
     * a deadlock between this call and an ongoing call to ikvdb_kvs_names_get()
     */
    kvdb_rest_deregister();

    mutex_lock(&self->ikdb_lock);

    for (i = 0; i < HSE_KVS_COUNT_MAX; i++) {
        struct kvdb_kvs *kvs = self->ikdb_kvs_vec[i];

        if (!kvs)
            continue;

        if (kvs->kk_ikvs)
            atomic_dec(&kvs->kk_refcnt);

        kvs_rest_deregister(kvs->kk_name);

        /* kvs_rest_deregister() waits until all active rest requests
         * have finished. Verify that the refcnt has gone down to zero
         */
        assert(atomic_read(&kvs->kk_refcnt) == 0);

        if (kvs->kk_ikvs) {
            err = kvs_close(kvs->kk_ikvs);
            if (ev(err))
                ret = ret ?: err;
        }

        self->ikdb_kvs_vec[i] = NULL;
        kvdb_kvs_destroy(kvs);
    }

    /* c0sk can only be closed after all c0s. This ensures that there are
     * no references to c0sk at this point.
     */
    if (self->ikdb_c0sk) {
        err = c0sk_close(self->ikdb_c0sk);
        if (ev(err))
            ret = ret ?: err;
    }

    /* Destroy LC only after c0sk has been destroyed. This ensures that the Garbage collector is
     * not running.
     */
    lc_destroy(self->ikdb_lc);
    self->ikdb_lc = NULL;

    cn_kvdb_destroy(self->ikdb_cn_kvdb);

    err = cndb_close(self->ikdb_cndb);
    if (ev(err))
        ret = ret ?: err;

    err = kvdb_log_close(self->ikdb_log);
    if (ev(err))
        ret = ret ?: err;

    wal_close(self->ikdb_wal);

    mutex_unlock(&self->ikdb_lock);

    ikvdb_txn_fini(self);

    kvdb_ctxn_set_destroy(self->ikdb_ctxn_set);

    c0snr_set_destroy(self->ikdb_c0snr_set);

    kvdb_keylock_destroy(self->ikdb_keylock);

    viewset_destroy(self->ikdb_cur_viewset);
    viewset_destroy(self->ikdb_txn_viewset);

    csched_destroy(self->ikdb_csched);

    mutex_destroy(&self->ikdb_lock);

    throttle_fini(&self->ikdb_throttle);

    ikvdb_perfc_free(self);

    free(self);

    return ret;
}

static void
ikvdb_throttle(struct ikvdb_impl *self, u64 bytes)
{
    u64 sleep_ns;

    sleep_ns = tbkt_request(&self->ikdb_tb, bytes);
    tbkt_delay(sleep_ns);

    if (self->ikdb_tb_dbg) {
        atomic64_inc(&self->ikdb_tb_dbg_ops);
        atomic64_add(bytes, &self->ikdb_tb_dbg_bytes);
        atomic64_add(sleep_ns, &self->ikdb_tb_dbg_sleep_ns);
    }
}

static inline bool
is_write_allowed(struct ikvs *kvs, struct hse_kvdb_txn *const txn)
{
    const bool kvs_is_txn = kvs_txn_is_enabled(kvs);
    const bool op_is_txn = txn;

    return kvs_is_txn ^ op_is_txn ? false : true;
}

static inline bool
is_read_allowed(struct ikvs *kvs, struct hse_kvdb_txn *const txn)
{
    return txn && !kvs_txn_is_enabled(kvs) ? false : true;
}

merr_t
ikvdb_kvs_put(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    struct kvs_ktuple *        kt,
    struct kvs_vtuple *        vt)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *parent;
    struct kvs_ktuple  ktbuf;
    struct kvs_vtuple  vtbuf;
    uint64_t           seqnoref;
    merr_t             err;
    uint               vlen, clen;
    size_t             vbufsz;
    void *             vbuf;

    if (ev(!handle))
        return merr(EINVAL);

    if (ev(!is_write_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    parent = kk->kk_parent;
    if (ev(parent->ikdb_rdonly))
        return merr(EROFS);

    /* puts do not stop on block deletion failures. */
    err = kvdb_health_check(
        &parent->ikdb_health, KVDB_HEALTH_FLAG_ALL & ~KVDB_HEALTH_FLAG_DELBLKFAIL);
    if (ev(err))
        return err;

    if (flags & HSE_FLAG_PUT_PRIORITY || parent->ikdb_rp.throttle_disable)
        parent = NULL;

    ktbuf = *kt;
    vtbuf = *vt;

    kt = &ktbuf;
    vt = &vtbuf;

    vlen = kvs_vtuple_vlen(vt);
    clen = kvs_vtuple_clen(vt);

    vbufsz = tls_vbufsz;
    vbuf = NULL;

    if ((clen == 0 && vlen > kk->kk_vcompmin && !(flags & HSE_FLAG_PUT_VALUE_COMPRESSION_OFF)) ||
        flags & HSE_FLAG_PUT_VALUE_COMPRESSION_ON) {
        if (vlen > kk->kk_vcompbnd) {
            vbufsz = vlen + PAGE_SIZE * 2;
            vbuf = vlb_alloc(vbufsz);
        } else {
            vbuf = tls_vbuf;
        }

        if (vbuf) {
            /* Currently kk_vcompress is not set in non-compressed KVSs. This
             * will change.
             */
            assert(kk->kk_vcompress);
            err = kk->kk_vcompress(vt->vt_data, vlen, vbuf, vbufsz, &clen);

            if (!err && clen < vlen) {
                kvs_vtuple_cinit(vt, vbuf, vlen, clen);
                vlen = clen;
            }
        }
    }

    seqnoref = txn ? 0 : HSE_SQNREF_SINGLE;

    err = kvs_put(kk->kk_ikvs, txn, kt, vt, seqnoref);

    if (vbuf && vbuf != tls_vbuf)
        vlb_free(vbuf, (vbufsz > VLB_ALLOCSZ_MAX) ? vbufsz : clen);

    if (parent)
        ikvdb_throttle(parent, kt->kt_len + (clen ? clen : vlen));

    return err;
}

merr_t
ikvdb_kvs_pfx_probe(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    struct kvs_ktuple *        kt,
    enum key_lookup_res *      res,
    struct kvs_buf *           kbuf,
    struct kvs_buf *           vbuf)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *p;
    u64                view_seqno;

    if (ev(!handle))
        return merr(EINVAL);

    if (ev(!is_read_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    p = kk->kk_parent;

    if (txn) {
        /*
         * No need to wait for ongoing commits. A transaction waited when its view was
         * being established i.e. at the time of transaction begin.
         */
        view_seqno = 0;
    } else {
        /* Establish our view before waiting on ongoing commits. */
        view_seqno = atomic64_read(&p->ikdb_seqno);
        kvdb_ctxn_set_wait_commits(p->ikdb_ctxn_set);
    }

    return kvs_pfx_probe(kk->kk_ikvs, txn, kt, view_seqno, res, kbuf, vbuf);
}

merr_t
ikvdb_kvs_get(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    struct kvs_ktuple *        kt,
    enum key_lookup_res *      res,
    struct kvs_buf *           vbuf)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *p;
    u64                view_seqno;

    if (ev(!handle))
        return merr(EINVAL);

    if (ev(!is_read_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    p = kk->kk_parent;

    if (txn) {
        /*
         * No need to wait for ongoing commits. A transaction waited when its view was
         * being established i.e. at the time of transaction begin.
         */
        view_seqno = 0;
    } else {
        /* Establish our view before waiting on ongoing commits. */
        view_seqno = atomic64_read(&p->ikdb_seqno);
        kvdb_ctxn_set_wait_commits(p->ikdb_ctxn_set);
    }

    return kvs_get(kk->kk_ikvs, txn, kt, view_seqno, res, vbuf);
}

merr_t
ikvdb_kvs_del(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    struct kvs_ktuple *        kt)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *parent;
    u64                seqnoref;
    merr_t             err;

    if (ev(!handle))
        return merr(EINVAL);

    if (ev(!is_write_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    parent = kk->kk_parent;
    if (ev(parent->ikdb_rdonly))
        return merr(EROFS);

    /* tombstone puts do not stop on block deletion failures. */
    err = kvdb_health_check(
        &parent->ikdb_health, KVDB_HEALTH_FLAG_ALL & ~KVDB_HEALTH_FLAG_DELBLKFAIL);
    if (ev(err))
        return err;

    seqnoref = txn ? 0 : HSE_SQNREF_SINGLE;

    return kvs_del(kk->kk_ikvs, txn, kt, seqnoref);
}

merr_t
ikvdb_kvs_prefix_delete(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    struct kvs_ktuple *        kt,
    size_t *                   kvs_pfx_len)
{
    struct kvdb_kvs *  kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *parent;
    u32                ct_pfx_len;
    u64                seqnoref;

    if (ev(!handle))
        return merr(EINVAL);

    if (ev(!is_write_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    parent = kk->kk_parent;
    if (ev(parent->ikdb_rdonly))
        return merr(EROFS);

    ct_pfx_len = kk->kk_cparams->pfx_len;
    if (kvs_pfx_len)
        *kvs_pfx_len = ct_pfx_len;

    if (ev(!kt->kt_data || kt->kt_len != ct_pfx_len))
        return merr(EINVAL);
    if (ev(kt->kt_len == 0))
        return merr(ENOENT);

    seqnoref = txn ? 0 : HSE_SQNREF_SINGLE;

    /* Prefix tombstone deletes all current keys with a matching prefix -
     * those with a sequence number up to but excluding the current seqno.
     * Insert prefix tombstone with a higher seqno. Use a higher sequence
     * number to allow newer mutations (after prefix) to be distinguished.
     */
    return kvs_prefix_del(kk->kk_ikvs, txn, kt, seqnoref);
}

/*-  IKVDB Cursors --------------------------------------------------*/

/*
 * IKVDB cursors allow iteration over a single KVS' c0, cN, and ctxn.
 * The normal life-cycle is create-iterate-destroy, where iterate has
 * several verbs: seek, read, bind, and update.  Cursors are single-threaded
 * and they are stateful.  These states are:
 *
 * 0 nil - cursor does not exist
 * 1 use - cursor has been created, and is iteratable
 * 2 err - cursor is in error and must be destroyed
 * 3 txn - cursor is bound to a transaction
 * 4 inv - cursor is invalid, either because the txn commited/aborted
 *         or because the cursor was holding onto resources too long
 *         and they were removed.
 *
 * These states are operated on by direct calls into kvdb, or indirectly
 * due to an asynchronous timeout, or an error resulting from a kvdb call.
 *
 * The state transition table (dashes represent invalid verbs for a state):
 *
 *              0/nil   1/use   2/err   3/txn   4/inv
 *      create  1       -       -       -       -
 *      destroy -       0       0       0       0
 *      update  -       1a      -       3b      1a
 *      bind    -       3c      -       -       3c
 *      commit  -       -       -       4       -
 *      abort   -       -       -       4       -
 *
 * a - view seqno is updated as in create
 * b - view seqno remains same, but all existing keys in txn become visible
 * c - view seqno is set to the transactions view
 *
 * Seek and read are available in states 1 and 3, and return ESTALE in 4.
 * They can only operate over the keys visible at the time of the create
 * or last update.
 *
 * State 2 can only occur if there is an error in an underlying operation.
 *
 * Transactions only interact with bound cursors (state 3); transaction
 * puts and dels after bind are invisible until a subsequent update,
 * just as puts and dels after create are invisible until an update.
 *
 * Both create and update may return EAGAIN.  This does not create an error
 * condition, as simply repeating the call may succeed.
 */

static void
cursor_view_release(struct hse_kvs_cursor *cursor)
{
    u64 minview;
    u32 minchg;

    if (!cursor->kc_on_list)
        return;

    viewset_remove(cursor->kc_kvs->kk_viewset, cursor->kc_viewcookie, &minchg, &minview);
    cursor->kc_on_list = false;
}

static merr_t
cursor_view_acquire(struct hse_kvs_cursor *cursor)
{
    merr_t err;

    /* Add to cursor list only if this is NOT part of a txn.
     */
    if (cursor->kc_seq != HSE_SQNREF_UNDEFINED)
        return 0;

    err = viewset_insert(cursor->kc_kvs->kk_viewset, &cursor->kc_seq, &cursor->kc_viewcookie);
    if (!err)
        cursor->kc_on_list = true;

    return err;
}

static merr_t
cursor_unbind_txn(struct hse_kvs_cursor *cur)
{
    struct kvdb_ctxn_bind *bind = cur->kc_bind;

    if (bind) {
        cur->kc_gen = -1;
        cur->kc_bind = 0;
        kvdb_ctxn_cursor_unbind(bind);
    }

    return 0;
}

merr_t
ikvdb_kvs_cursor_create(
    struct hse_kvs *           handle,
    const unsigned int         flags,
    struct hse_kvdb_txn *const txn,
    const void *               prefix,
    size_t                     pfx_len,
    struct hse_kvs_cursor **   cursorp)
{
    struct kvdb_kvs *      kk = (struct kvdb_kvs *)handle;
    struct ikvdb_impl *    ikvdb = kk->kk_parent;
    struct kvdb_ctxn *     ctxn = 0;
    struct hse_kvs_cursor *cur = 0;
    merr_t                 err;
    u64                    ts, vseq, tstart;
    struct perfc_set *     pkvsl_pc;

    *cursorp = NULL;

    if (ev(!is_read_allowed(kk->kk_ikvs, txn)))
        return merr(EINVAL);

    if (ev(atomic_read(&ikvdb->ikdb_curcnt) > ikvdb->ikdb_curcnt_max))
        return merr(ECANCELED);

    pkvsl_pc = kvs_perfc_pkvsl(kk->kk_ikvs);
    tstart = perfc_lat_start(pkvsl_pc);

    vseq = HSE_SQNREF_UNDEFINED;

    if (txn) {
        ctxn = kvdb_ctxn_h2h(txn);
        err = kvdb_ctxn_get_view_seqno(ctxn, &vseq);
        if (ev(err))
            return err;
    }

    /* The initialization sequence is driven by the way the sequence
     * number horizon is tracked, which requires atomically getting a
     * cursor's view sequence number and inserting the cursor at the head
     * of the list of cursors.  This must be done prior to cursor
     * creation, hence the need to separate cursor alloc from cursor
     * init/create.  The steps are:
     *  - allocate cursor struct
     *  - register cursor (atomic get seqno, add to kk_cursors)
     *  - initialize cursor
     * The failure path must unregister the cursor from kk_cursors.
     */
    cur = kvs_cursor_alloc(kk->kk_ikvs, prefix, pfx_len, flags & HSE_FLAG_CURSOR_REVERSE);
    if (ev(!cur))
        return merr(ENOMEM);

    cur->kc_pkvsl_pc = pkvsl_pc;

    /* if we have a transaction at all, use its view seqno... */
    cur->kc_seq = vseq;
    cur->kc_flags = flags;

    cur->kc_kvs = kk;
    cur->kc_gen = 0;
    cur->kc_ctxn = ctxn;
    cur->kc_bind = ctxn ? kvdb_ctxn_cursor_bind(ctxn) : NULL;

    /* Temporarily lock a view until this cursor gets refs on cn kvsets. */
    err = cursor_view_acquire(cur);
    if (ev(err))
        goto out;

    ts = perfc_lat_start(pkvsl_pc);
    err = kvs_cursor_init(cur, ctxn);
    perfc_lat_record(pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_INIT, ts);
    if (ev(err))
        goto out;

    cursor_view_release(cur); /* release the view that was locked */

    /* After acquiring a view, non-txn cursors must wait for ongoing commits
     * to finish to ensure they never see partial txns.  This is not necessary
     * for txn cursors because their view is inherited from the txn.
     */
    if (!txn)
        kvdb_ctxn_set_wait_commits(ikvdb->ikdb_ctxn_set);

    err = kvs_cursor_prepare(cur);
    if (ev(err))
        goto out;

    perfc_inc(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_CURCNT);
    cur->kc_create_time = tstart;

    perfc_lat_record(pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_CREATE, tstart);

    *cursorp = cur;

out:
    if (err)
        ikvdb_kvs_cursor_destroy(cur);

    return err;
}

merr_t
ikvdb_kvs_cursor_update_view(struct hse_kvs_cursor *cur, unsigned int flags)
{
    merr_t err;
    u64    tstart;

    tstart = perfc_lat_start(cur->kc_pkvsl_pc);

    /* a cursor in error cannot be updated - must be destroyed */
    if (ev(cur->kc_err))
        return cur->kc_err;

    if (ev(cur->kc_ctxn))
        return merr(EINVAL);

    cur->kc_seq = HSE_SQNREF_UNDEFINED;

    /* Temporarily reserve seqno until this cursor gets refs on
     * cn kvsets.
     */
    err = cursor_view_acquire(cur);
    if (ev(err))
        return err;

    cur->kc_err = kvs_cursor_update(cur, NULL, cur->kc_seq);
    if (ev(cur->kc_err))
        goto out;

    cursor_view_release(cur);

    /* After acquiring a view, non-txn cursors must wait for ongoing commits
     * to finish to ensure they never see partial txns.
     */
    kvdb_ctxn_set_wait_commits(cur->kc_kvs->kk_parent->ikdb_ctxn_set);

    cur->kc_flags = flags;

    perfc_lat_record(cur->kc_pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_UPDATE, tstart);

out:
    /* Since the update code doesn't currently allow retrying, change the error code
     * if it's an EAGAIN. Wherever possible, the code retries the cursor update call
     * internally.
     */
    if (ev(merr_errno(cur->kc_err) == EAGAIN))
        cur->kc_err = merr(ENOTRECOVERABLE);

    return ev(cur->kc_err);
}

static merr_t
cursor_refresh(struct hse_kvs_cursor *cur)
{
    struct kvdb_ctxn_bind *bind = cur->kc_bind;
    merr_t                 err = 0;
    int                    up = 0;

    if (!bind->b_ctxn) {
        /* canceled: txn was committed or aborted since last look */
        err = cursor_unbind_txn(cur);
        if (ev(err))
            return err;
        ++up;

    } else if (atomic64_read(&bind->b_gen) != cur->kc_gen) {
        /* stale or canceled: txn was updated since last look */
        ++up;
    }

    if (up)
        err = kvs_cursor_update(cur, cur->kc_bind ? cur->kc_bind->b_ctxn : 0, cur->kc_seq);

    return ev(err);
}

merr_t
ikvdb_kvs_cursor_seek(
    struct hse_kvs_cursor *cur,
    const unsigned int     flags,
    const void *           key,
    size_t                 len,
    const void *           limit,
    size_t                 limit_len,
    struct kvs_ktuple *    kt)
{
    merr_t err;
    u64    tstart;

    tstart = perfc_lat_start(cur->kc_pkvsl_pc);

    if (ev(limit && (cur->kc_flags & HSE_FLAG_CURSOR_REVERSE)))
        return merr(EINVAL);

    if (ev(cur->kc_err)) {
        if (ev(merr_errno(cur->kc_err) != EAGAIN))
            return cur->kc_err;

        cur->kc_err = kvs_cursor_update(cur, cur->kc_bind ? cur->kc_bind->b_ctxn : 0, cur->kc_seq);
        if (ev(cur->kc_err))
            return cur->kc_err;
    }

    if (cur->kc_bind) {
        cur->kc_err = cursor_refresh(cur);
        if (ev(cur->kc_err))
            return cur->kc_err;
    }

    /* errors on seek are not fatal */
    err = kvs_cursor_seek(cur, key, (u32)len, limit, (u32)limit_len, kt);

    perfc_lat_record(cur->kc_pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_SEEK, tstart);

    return ev(err);
}

merr_t
ikvdb_kvs_cursor_read(
    struct hse_kvs_cursor *cur,
    const unsigned int     flags,
    const void **          key,
    size_t *               key_len,
    const void **          val,
    size_t *               val_len,
    bool *                 eof)
{
    struct kvs_kvtuple kvt;
    merr_t             err;
    u64                tstart;

    tstart = perfc_lat_start(cur->kc_pkvsl_pc);

    if (ev(cur->kc_err)) {
        if (ev(merr_errno(cur->kc_err) != EAGAIN))
            return cur->kc_err;

        cur->kc_err = kvs_cursor_update(cur, cur->kc_bind ? cur->kc_bind->b_ctxn : 0, cur->kc_seq);
        if (ev(cur->kc_err))
            return cur->kc_err;
    }

    if (cur->kc_bind) {
        cur->kc_err = cursor_refresh(cur);
        if (ev(cur->kc_err))
            return cur->kc_err;
    }

    err = kvs_cursor_read(cur, &kvt, eof);
    if (ev(err))
        return err;
    if (*eof)
        return 0;

    *key = kvt.kvt_key.kt_data;
    *key_len = kvt.kvt_key.kt_len;

    *val = kvt.kvt_value.vt_data;
    *val_len = kvs_vtuple_vlen(&kvt.kvt_value);

    perfc_lat_record(
        cur->kc_pkvsl_pc,
        cur->kc_flags & HSE_FLAG_CURSOR_REVERSE ? PERFC_LT_PKVSL_KVS_CURSOR_READREV
                                                : PERFC_LT_PKVSL_KVS_CURSOR_READFWD,
        tstart);

    return 0;
}

merr_t
ikvdb_kvs_cursor_destroy(struct hse_kvs_cursor *cur)
{
    struct perfc_set *pkvsl_pc;
    u64               tstart, ctime;

    if (!cur)
        return 0;

    pkvsl_pc = cur->kc_pkvsl_pc;
    tstart = perfc_lat_start(pkvsl_pc);
    ctime = cur->kc_create_time;

    cursor_unbind_txn(cur);

    perfc_dec(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_CURCNT);

    kvs_cursor_free(cur);

    perfc_lat_record(pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_DESTROY, tstart);
    perfc_lat_record(pkvsl_pc, PERFC_LT_PKVSL_KVS_CURSOR_FULL, ctime);

    return 0;
}

void
ikvdb_compact(struct ikvdb *handle, int flags)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    if (ev(self->ikdb_rdonly))
        return;

    csched_compact_request(self->ikdb_csched, flags);
}

void
ikvdb_compact_status_get(struct ikvdb *handle, struct hse_kvdb_compact_status *status)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    if (ev(self->ikdb_rdonly))
        return;

    csched_compact_status_get(self->ikdb_csched, status);
}

merr_t
ikvdb_sync(struct ikvdb *handle, const unsigned int flags)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    if (ev(self->ikdb_rdonly))
        return merr(EROFS);

    if (self->ikdb_wal)
        return wal_sync(self->ikdb_wal);

    return c0sk_sync(self->ikdb_c0sk, flags);
}

u64
ikvdb_horizon(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    u64                horizon;
    u64                b, c;

    b = viewset_horizon(self->ikdb_cur_viewset);
    c = viewset_horizon(self->ikdb_txn_viewset);

    horizon = min_t(u64, b, c);

    if (HSE_UNLIKELY(perfc_ison(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_SEQNO))) {
        u64 a;

        /* Must read a after b and c to test assertions. */
        __atomic_thread_fence(__ATOMIC_RELEASE);

        a = atomic64_read(&self->ikdb_seqno);
        assert(b == U64_MAX || a >= b);
        assert(a >= c);

        perfc_set(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_SEQNO, a);
        perfc_set(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_CURHORIZON, b);
        perfc_set(&kvdb_metrics_pc, PERFC_BA_KVDBMETRICS_HORIZON, horizon);
    }

    return horizon;
}

u64
ikvdb_txn_horizon(struct ikvdb *handle)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);

    return viewset_horizon(self->ikdb_txn_viewset);
}


static HSE_ALWAYS_INLINE struct kvdb_ctxn_bkt *
ikvdb_txn_tid2bkt(struct ikvdb_impl *self)
{
    u64 tid = pthread_self();

    return self->ikdb_ctxn_cache + (tid % NELEM(self->ikdb_ctxn_cache));
}

struct hse_kvdb_txn *
ikvdb_txn_alloc(struct ikvdb *handle)
{
    struct ikvdb_impl *   self = ikvdb_h2r(handle);
    struct kvdb_ctxn_bkt *bkt = ikvdb_txn_tid2bkt(self);
    struct kvdb_ctxn *    ctxn = NULL;

    spin_lock(&bkt->kcb_lock);
    if (bkt->kcb_ctxnc > 0)
        ctxn = bkt->kcb_ctxnv[--bkt->kcb_ctxnc];
    spin_unlock(&bkt->kcb_lock);

    if (ctxn) {
        kvdb_ctxn_reset(ctxn);
        return &ctxn->ctxn_handle;
    }

    ctxn = kvdb_ctxn_alloc(
        self->ikdb_keylock,
        &self->ikdb_seqno,
        self->ikdb_ctxn_set,
        self->ikdb_txn_viewset,
        self->ikdb_c0snr_set,
        self->ikdb_c0sk,
        self->ikdb_wal);
    if (ev(!ctxn))
        return NULL;

    perfc_inc(&self->ikdb_ctxn_op, PERFC_RA_CTXNOP_ALLOC);

    return &ctxn->ctxn_handle;
}

void
ikvdb_txn_free(struct ikvdb *handle, struct hse_kvdb_txn *txn)
{
    struct ikvdb_impl *   self = ikvdb_h2r(handle);
    struct kvdb_ctxn_bkt *bkt = ikvdb_txn_tid2bkt(self);
    struct kvdb_ctxn *    ctxn;

    if (!txn)
        return;

    ctxn = kvdb_ctxn_h2h(txn);
    kvdb_ctxn_abort(ctxn);

    spin_lock(&bkt->kcb_lock);
    if (bkt->kcb_ctxnc < NELEM(bkt->kcb_ctxnv)) {
        bkt->kcb_ctxnv[bkt->kcb_ctxnc++] = ctxn;
        ctxn = NULL;
    }
    spin_unlock(&bkt->kcb_lock);

    if (ctxn) {
        perfc_inc(&self->ikdb_ctxn_op, PERFC_RA_CTXNOP_FREE);

        kvdb_ctxn_free(ctxn);
    }
}

merr_t
ikvdb_txn_begin(struct ikvdb *handle, struct hse_kvdb_txn *txn)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct kvdb_ctxn  *ctxn = kvdb_ctxn_h2h(txn);
    merr_t             err;

    perfc_inc(&self->ikdb_ctxn_op, PERFC_BA_CTXNOP_ACTIVE);
    perfc_inc(&self->ikdb_ctxn_op, PERFC_RA_CTXNOP_BEGIN);

    err = kvdb_ctxn_begin(ctxn);
    if (err)
        perfc_dec(&self->ikdb_ctxn_op, PERFC_BA_CTXNOP_ACTIVE);

    return err;
}

merr_t
ikvdb_txn_commit(struct ikvdb *handle, struct hse_kvdb_txn *txn)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct kvdb_ctxn  *ctxn = kvdb_ctxn_h2h(txn);
    merr_t             err;
    u64                lstart;

    lstart = perfc_lat_startu(&self->ikdb_ctxn_op, PERFC_LT_CTXNOP_COMMIT);
    perfc_inc(&self->ikdb_ctxn_op, PERFC_RA_CTXNOP_COMMIT);

    err = kvdb_ctxn_commit(ctxn);

    perfc_dec(&self->ikdb_ctxn_op, PERFC_BA_CTXNOP_ACTIVE);
    perfc_lat_record(&self->ikdb_ctxn_op, PERFC_LT_CTXNOP_COMMIT, lstart);

    return err;
}

merr_t
ikvdb_txn_abort(struct ikvdb *handle, struct hse_kvdb_txn *txn)
{
    struct ikvdb_impl *self = ikvdb_h2r(handle);
    struct kvdb_ctxn  *ctxn = kvdb_ctxn_h2h(txn);

    perfc_inc(&self->ikdb_ctxn_op, PERFC_RA_CTXNOP_ABORT);

    kvdb_ctxn_abort(ctxn);

    perfc_dec(&self->ikdb_ctxn_op, PERFC_BA_CTXNOP_ACTIVE);

    return 0;
}

enum kvdb_ctxn_state
ikvdb_txn_state(struct ikvdb *handle, struct hse_kvdb_txn *txn)
{
    return kvdb_ctxn_get_state(kvdb_ctxn_h2h(txn));
}

/*-  Perf Counter Support  --------------------------------------------------*/

/*
 * Perf counters, once allocated, are only released upon module fini.
 * This preserves the user-space counters until they can be emitted,
 * and allows counters to be accumulated in use cases where multiple
 * open/close per application lifetime are employed.
 *
 * Therefore, the pointers to the allocated counters (cf cn_perfc_alloc())
 * are remembered here, and released after emitting.  It is possible for
 * an application to open several different datasets, each with its own
 * set of perf counters.  All these are remembered, then emitted and
 * released here.
 *
 * The intervals used by the perf counters are customized once here,
 * then set in the static structures at init time.
 *
 * Finally, there are a couple of configurable items set here:
 *      1. Should hse messages be sent to stderr?
 *      2. Are perf counters enabled?
 *
 * The only public APIs is:
 *      void kvdb_perfc_register(void *);
 */

static struct darray kvdb_perfc_reg;

/*
 * kvdb_perfc_register - remember this perfc pointer until module fini
 *
 * NB: It is NOT fatal to have an error here.  It simply means the
 * memory will not be freed on module fini.
 */
void
kvdb_perfc_register(void *pc)
{
    if (darray_append_uniq(&kvdb_perfc_reg, pc) != 0)
        hse_log(
            HSE_ERR "kvdb_perfc_register: cannot register"
                    " perf counter #%d for %s",
            kvdb_perfc_reg.cur + 1,
            perfc_ctrseti_path(pc));
}

/*
 * This function is called once at constructor time.
 * The variables that control log verbosity and perf counters
 * must be set at compile time -- there is no before-this
 * configuration to change at this point.
 *
 * However, setter methods are available from this point
 * forward, so these defaults can be overridden programatically.
 */

static void
kvdb_perfc_initialize(void)
{
    perfc_verbosity = 2;

    kvdb_perfc_init();
    kvs_perfc_init();
    c0sk_perfc_init();
    cn_perfc_init();
    throttle_perfc_init();

    hse_openlog(COMPNAME, 0);

    if (perfc_ctrseti_alloc(COMPNAME, "global", kvdb_perfc_op, PERFC_EN_KVDBOP, "set", &kvdb_pc))
        hse_log(HSE_ERR "cannot alloc kvdb op perf counters");
    else
        kvdb_perfc_register(&kvdb_pc);

    if (perfc_ctrseti_alloc(
            COMPNAME, "global", kvdb_perfc_pkvdbl_op, PERFC_EN_PKVDBL, "set", &kvdb_pkvdbl_pc))
        hse_log(HSE_ERR "cannot alloc kvdb public op perf counters");
    else
        kvdb_perfc_register(&kvdb_pkvdbl_pc);

    if (perfc_ctrseti_alloc(
            COMPNAME, "global", c0_metrics_perfc, PERFC_EN_C0METRICS, "set", &c0_metrics_pc))
        hse_log(HSE_ERR "cannot alloc c0 metrics perf counters");
    else
        kvdb_perfc_register(&c0_metrics_pc);

    if (perfc_ctrseti_alloc(
            COMPNAME, "global", kvdb_metrics_perfc, PERFC_EN_KVDBMETRICS, "set", &kvdb_metrics_pc))
        hse_log(HSE_ERR "cannot alloc kvdb metrics perf counters");
    else
        kvdb_perfc_register(&kvdb_metrics_pc);
}

static void
kvdb_perfc_finish(void)
{
    darray_apply(&kvdb_perfc_reg, (darray_func)perfc_ctrseti_free);
    darray_fini(&kvdb_perfc_reg);

    throttle_perfc_fini();
    cn_perfc_fini();
    c0sk_perfc_fini();
    kvs_perfc_fini();
    kvdb_perfc_fini();
}

/* Called once by load() at program start or module load time.
 */
merr_t
ikvdb_init(void)
{
    merr_t err;

    kvdb_perfc_initialize();

    kvs_init();

    err = c0_init();
    if (err)
        goto errout;

    err = lc_init();
    if (err) {
        c0_fini();
        goto errout;
    }

    err = cn_init();
    if (err) {
        lc_fini();
        c0_fini();
        goto errout;
    }

    err = bkv_collection_init();
    if (err) {
        cn_fini();
        lc_fini();
        c0_fini();
        goto errout;
    }

errout:
    if (err) {
        kvs_fini();
        kvdb_perfc_finish();
    }

    return err;
}

/* Called once by unload() at program termination or module unload time.
 */
void
ikvdb_fini(void)
{
    bkv_collection_fini();
    cn_fini();
    lc_fini();
    c0_fini();
    kvs_fini();
    kvdb_perfc_finish();
}

#if HSE_MOCKING
#include "ikvdb_ut_impl.i"
#include "kvs_ut_impl.i"
#endif /* HSE_MOCKING */
