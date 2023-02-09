/* SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * SPDX-FileCopyrightText: Copyright 2021 Micron Technology, Inc.
 */

#ifndef MPOOL_MDC_H
#define MPOOL_MDC_H

#include <hse/error/merr.h>
#include <hse/util/mutex.h>

#define MDC_FILES_MAX       10
#define MDC_NAME_LENGTH_MAX 128

struct mpool;
struct mpool_mdc;

/**
 * mdc_mclass_get() - get mclass handle
 *
 * @mdc: MDC handle
 */
struct media_class *
mdc_mclass_get(struct mpool_mdc *mdc);

#endif /* MPOOL_MDC_H */
