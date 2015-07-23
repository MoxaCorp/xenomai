/*
 * Copyright (C) 2005 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */
#pragma GCC system_header
#include_next <sched.h>

#ifndef _COBALT_SCHED_H
#define _COBALT_SCHED_H

#include <sys/types.h>
#include <cobalt/wrappers.h>
#include <cobalt/uapi/sched.h>

COBALT_DECL(int, sched_yield(void));

COBALT_DECL(int, sched_get_priority_min(int policy));

COBALT_DECL(int, sched_get_priority_max(int policy));

#ifdef __cplusplus
extern "C" {
#endif

int sched_get_priority_min_ex(int policy);

int sched_get_priority_max_ex(int policy);

int sched_setconfig_np(int cpu, int policy,
		       const union sched_config *config, size_t len);

ssize_t sched_getconfig_np(int cpu, int policy,
			   union sched_config *config, size_t *len_r);

#ifdef __cplusplus
}
#endif

#endif /* !_COBALT_SCHED_H */
