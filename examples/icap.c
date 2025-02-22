/*
 * Copyright 2016-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * icap.c -- tiny syscall intercepting example library, turning every
 * lower case letter 'i' to on upper case 'I' in buffers used in write syscalls.
 */
#include <stddef.h>
#include <string.h>
#include <syscall.h>

#include "libsyscall_intercept_hook_point.h"

static int hook(long syscall_number,
		long arg0, long arg1,
		long arg2, long arg3,
		long arg4, long arg5,
		long *result)
{
	(void) arg3;
	(void) arg4;
	(void) arg5;
	struct wrapper_ret ret;

	if (syscall_number == SYS_write) {
		char buf_copy[0x1000];
		size_t size = (size_t)arg2;

		if (size > sizeof(buf_copy))
			size = sizeof(buf_copy);

		memcpy(buf_copy, (char *)arg1, size);

		/* Capitalize the letter 'i', for fun */
		for (size_t i = 0; i < size; ++i) {
			if (buf_copy[i] == 'i')
				buf_copy[i] = 'I';
		}
		ret = syscall_no_intercept(SYS_write, arg0, buf_copy, size);
		*result = ret.a0;
		return 0;
	}
	return 1;
}

static __attribute__((constructor)) void
start(void)
{
	intercept_hook_point = &hook;
}
