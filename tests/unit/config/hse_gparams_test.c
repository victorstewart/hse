/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2021 Micron Technology, Inc.  All rights reserved.
 */

#include <hse_ut/framework.h>

#include <hse_util/vlb.h>

#include <hse_ikvdb/argv.h>
#include <hse_ikvdb/limits.h>
#include <hse_ikvdb/hse_gparams.h>

#include <stdarg.h>

MTF_BEGIN_UTEST_COLLECTION(hse_gparams_test)

struct hse_gparams params;

int
test_pre(struct mtf_test_info *ti)
{
    params = hse_gparams_defaults();

	return 0;
}

const struct param_spec *
ps_get(const char *const name)
{
	size_t                   sz = 0;
	const struct param_spec *pspecs = hse_gparams_pspecs_get(&sz);

	assert(name);

	for (size_t i = 0; i < sz; i++) {
		if (!strcmp(pspecs[i].ps_name, name))
			return &pspecs[i];
	}

	return NULL;
}

/**
 * Check the validity of various key=value combinations
 */
merr_t HSE_SENTINEL
check(const char *const arg, ...)
{
    merr_t      err;
    bool        success;
    const char *a = arg;
    va_list     ap;

	assert(arg);

    va_start(ap, arg);

	do {
        const char * paramv[] = { a };
        const size_t paramc = NELEM(paramv);

		success = !!va_arg(ap, int);

        err = argv_deserialize_to_hse_gparams(paramc, paramv, &params);

        if (success != !err) {
            break;
		} else {
			/* Reset err because we expected it */
			err = 0;
		}
	} while ((a = va_arg(ap, char *)));

    va_end(ap);

	return err;
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, c0kvs_ccache_sz_max, test_pre)
{
	const struct param_spec *ps = ps_get("c0kvs_ccache_sz_max");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(PARAM_FLAG_EXPERIMENTAL, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_U64, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(HSE_C0_CCACHE_SZ_DFLT, params.gp_c0kvs_ccache_sz);
	ASSERT_EQ(0, ps->ps_bounds.as_uscalar.ps_min);
	ASSERT_EQ(HSE_C0_CCACHE_SZ_MAX, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, c0kvs_ccache_sz, test_pre)
{
	const struct param_spec *ps = ps_get("c0kvs_ccache_sz");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(PARAM_FLAG_EXPERIMENTAL, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_U64, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(HSE_C0_CCACHE_SZ_DFLT, params.gp_c0kvs_ccache_sz);
	ASSERT_EQ(0, ps->ps_bounds.as_uscalar.ps_min);
	ASSERT_EQ(HSE_C0_CCACHE_SZ_MAX, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, c0kvs_cheap_sz, test_pre)
{
	const struct param_spec *ps = ps_get("c0kvs_cheap_sz");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(PARAM_FLAG_EXPERIMENTAL, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_U64, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(HSE_C0_CHEAP_SZ_DFLT, params.gp_c0kvs_cheap_sz);
	ASSERT_EQ(HSE_C0_CHEAP_SZ_MIN, ps->ps_bounds.as_uscalar.ps_min);
	ASSERT_EQ(HSE_C0_CHEAP_SZ_MAX, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, vlb_cache_sz, test_pre)
{
    const struct param_spec *ps = ps_get("vlb_cache_sz");

    ASSERT_NE(NULL, ps);
    ASSERT_NE(NULL, ps->ps_description);
    ASSERT_EQ(PARAM_FLAG_EXPERIMENTAL, ps->ps_flags);
    ASSERT_EQ(PARAM_TYPE_U64, ps->ps_type);
    ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
    ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
    ASSERT_EQ(HSE_VLB_CACHESZ_DFLT, params.gp_vlb_cache_sz);
    ASSERT_EQ(HSE_VLB_CACHESZ_MIN, ps->ps_bounds.as_uscalar.ps_min);
    ASSERT_EQ(HSE_VLB_CACHESZ_MAX, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, socket_enabled, test_pre)
{
	const struct param_spec *ps = ps_get("socket.enabled");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_BOOL, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(true, params.gp_socket.enabled);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, socket_path, test_pre)
{
	merr_t                   err;
	const struct param_spec *ps = ps_get("socket.path");

	char buf[sizeof(params.gp_socket.path)];

	snprintf(buf, sizeof(buf), "/tmp/hse-%d.sock", getpid());

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(PARAM_FLAG_DEFAULT_BUILDER, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_STRING, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_STREQ(buf, params.gp_socket.path);
	ASSERT_EQ(sizeof(((struct sockaddr_un *)0)->sun_path), ps->ps_bounds.as_string.ps_max_len);

	err = check("socket.path=null", false, NULL);
	ASSERT_EQ(0, err);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_enabled, test_pre)
{
	const struct param_spec *ps = ps_get("logging.enabled");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_BOOL, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(true, params.gp_socket.enabled);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_structured, test_pre)
{
	const struct param_spec *ps = ps_get("logging.structured");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_BOOL, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(false, params.gp_logging.structured);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_destination, test_pre)
{
	merr_t                   err;
	const struct param_spec *ps = ps_get("logging.destination");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_ENUM, ps->ps_type);
	ASSERT_NE((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(LD_SYSLOG, params.gp_logging.destination);

	/* clang-format off */
	err = check(
		"logging.destination=x", false,
		"logging.destination=stderr", true,
		"logging.destination=stdout", true,
		"logging.destination=syslog", true,
		"logging.destination=file", true,
		NULL
	);
	/* clang-format on */

	ASSERT_EQ(0, merr_errno(err));
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_level, test_pre)
{
	const struct param_spec *ps = ps_get("logging.level");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_ENUM, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(HSE_LOG_PRI_DEFAULT, params.gp_logging.level);
	ASSERT_EQ(HSE_EMERG_VAL, ps->ps_bounds.as_uscalar.ps_min);
	ASSERT_EQ(HSE_DEBUG_VAL, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_squelch_ns, test_pre)
{
	const struct param_spec *ps = ps_get("logging.squelch_ns");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(PARAM_FLAG_EXPERIMENTAL, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_U64, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_EQ(HSE_LOG_SQUELCH_NS_DEFAULT, params.gp_logging.squelch_ns);
	ASSERT_EQ(0, ps->ps_bounds.as_uscalar.ps_min);
	ASSERT_EQ(UINT64_MAX, ps->ps_bounds.as_uscalar.ps_max);
}

MTF_DEFINE_UTEST_PRE(hse_gparams_test, logging_path, test_pre)
{
	merr_t                   err;
	const struct param_spec *ps = ps_get("logging.path");

	ASSERT_NE(NULL, ps);
	ASSERT_NE(NULL, ps->ps_description);
	ASSERT_EQ(0, ps->ps_flags);
	ASSERT_EQ(PARAM_TYPE_STRING, ps->ps_type);
	ASSERT_EQ((uintptr_t)ps->ps_convert, (uintptr_t)param_default_converter);
	ASSERT_EQ((uintptr_t)ps->ps_validate, (uintptr_t)param_default_validator);
	ASSERT_STREQ("hse.log", params.gp_logging.path);
	ASSERT_EQ(PATH_MAX, ps->ps_bounds.as_string.ps_max_len);

	err = check("logging.path=null", false, NULL);
	ASSERT_EQ(0, err);
}

MTF_END_UTEST_COLLECTION(hse_gparams_test)