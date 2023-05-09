/*
 * Copyright (C) 2016-2021 Canonical, Ltd.
 * Copyright (C) 2021-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-builtin.h"
#include "core-cpu-cache.h"
#include "core-nt-store.h"
#include "core-target-clones.h"
#include "core-vecmath.h"

#define MR_RD			(0)
#define MR_WR			(1)

#define MIN_MEMRATE_BYTES       (4 * KB)
#define MAX_MEMRATE_BYTES       (MAX_MEM_LIMIT)
#define DEFAULT_MEMRATE_BYTES   (256 * MB)
#define STRESS_MEMRATE_PF_OFFSET (2 * KB)

#define STRESS_PTR_MINIMUM(a, b)	STRESS_MINIMUM((uintptr_t)a, (uintptr_t)b)

static const stress_help_t help[] = {
	{ NULL,	"memrate N",		"start N workers exercised memory read/writes" },
	{ NULL,	"memrate-bytes N",	"size of memory buffer being exercised" },
	{ NULL,	"memrate-ops N",	"stop after N memrate bogo operations" },
	{ NULL,	"memrate-rd-mbs N",	"read rate from buffer in megabytes per second" },
	{ NULL,	"memrate-wr-mbs N",	"write rate to buffer in megabytes per second" },
	{ NULL,	"memrate-flush",	"flush cache before each iteration" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_VECMATH)
typedef uint64_t stress_uint32w1024_t	__attribute__ ((vector_size(1024 / 8)));
typedef uint64_t stress_uint32w512_t	__attribute__ ((vector_size(512 / 8)));
typedef uint64_t stress_uint32w256_t	__attribute__ ((vector_size(256 / 8)));
typedef uint64_t stress_uint32w128_t	__attribute__ ((vector_size(128 / 8)));
#endif

static sigjmp_buf jmpbuf;

typedef struct {
	double		duration;
	double		kbytes;
	bool		valid;
} stress_memrate_stats_t;

typedef struct {
	stress_memrate_stats_t *stats;
	uint64_t memrate_bytes;
	uint64_t memrate_rd_mbs;
	uint64_t memrate_wr_mbs;
	void *start;
	void *end;
	bool memrate_flush;
} stress_memrate_context_t;

typedef uint64_t (*stress_memrate_func_t)(const stress_memrate_context_t *context, bool *valid);

typedef struct {
	const char 	*name;
	const int	rdwr;
	stress_memrate_func_t	func;
	stress_memrate_func_t	func_rate;
} stress_memrate_info_t;

static int stress_set_memrate_bytes(const char *opt)
{
	uint64_t memrate_bytes;

	memrate_bytes = stress_get_uint64_byte(opt);
	stress_check_range_bytes("memrate-bytes", memrate_bytes,
		MIN_MEMRATE_BYTES, MAX_MEMRATE_BYTES);
	return stress_set_setting("memrate-bytes", TYPE_ID_UINT64, &memrate_bytes);
}

static int stress_set_memrate_rd_mbs(const char *opt)
{
	uint64_t memrate_rd_mbs;

	memrate_rd_mbs = stress_get_uint64(opt);
	stress_check_range_bytes("memrate-rd-mbs", memrate_rd_mbs,
		1, 1000000);
	return stress_set_setting("memrate-rd-mbs", TYPE_ID_UINT64, &memrate_rd_mbs);
}

static int stress_set_memrate_wr_mbs(const char *opt)
{
	uint64_t memrate_wr_mbs;

	memrate_wr_mbs = stress_get_uint64(opt);
	stress_check_range_bytes("memrate-wr-mbs", memrate_wr_mbs,
		1, 1000000);
	return stress_set_setting("memrate-wr-mbs", TYPE_ID_UINT64, &memrate_wr_mbs);
}

static void NORETURN MLOCKED_TEXT stress_memrate_alarm_handler(int signum)
{
        (void)signum;
        siglongjmp(jmpbuf, 1);
}

static int stress_set_memrate_flush(const char *opt)
{
	return stress_set_setting_true("memrate-flush", opt);
}

static uint64_t stress_memrate_loops(
	const stress_memrate_context_t *context,
	const size_t size)
{
	uint64_t chunk_shift = 20;	/* 1 MB */
	uint64_t bytes = context->memrate_bytes;
	uint64_t best_fit = bytes / size;

	/* check for powers of 2 size, from 1MB down to 1K */
	for (chunk_shift = 20; chunk_shift >= 10; chunk_shift--) {
		if (((bytes >> chunk_shift) << chunk_shift) == bytes) {
			uint64_t n = 1ULL << chunk_shift;

			if (n <= best_fit)
				return n;
		}
	}
	/* best fit on non-power of 2 */
	return best_fit;
}

static void OPTIMIZE3 stress_memrate_flush(const stress_memrate_context_t *context)
{
	uint8_t *start ALIGNED(4096) = (uint8_t *)context->start;
	uint8_t *end ALIGNED(4096) = (uint8_t *)context->end;

	while (start < end) {
		shim_clflush(start);
		start += 64;
	}
}

#define STRESS_MEMRATE_READ(size, type, prefetch)		\
static uint64_t TARGET_CLONES OPTIMIZE3 stress_memrate_read##size(		\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	register type v, *ptr;					\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
								\
	for (ptr = start; ptr < end;) {				\
		prefetch((uint8_t *)ptr + STRESS_MEMRATE_PF_OFFSET, 0, 3);	\
		v = *(volatile type *)&ptr[0];			\
		(void)v;					\
		v = *(volatile type *)&ptr[1];			\
		(void)v;					\
		v = *(volatile type *)&ptr[2];			\
		(void)v;					\
		v = *(volatile type *)&ptr[3];			\
		(void)v;					\
		v = *(volatile type *)&ptr[4];			\
		(void)v;					\
		v = *(volatile type *)&ptr[5];			\
		(void)v;					\
		v = *(volatile type *)&ptr[6];			\
		(void)v;					\
		v = *(volatile type *)&ptr[7];			\
		(void)v;					\
		v = *(volatile type *)&ptr[8];			\
		(void)v;					\
		v = *(volatile type *)&ptr[9];			\
		(void)v;					\
		v = *(volatile type *)&ptr[10];			\
		(void)v;					\
		v = *(volatile type *)&ptr[11];			\
		(void)v;					\
		v = *(volatile type *)&ptr[12];			\
		(void)v;					\
		v = *(volatile type *)&ptr[13];			\
		(void)v;					\
		v = *(volatile type *)&ptr[14];			\
		(void)v;					\
		v = *(volatile type *)&ptr[15];			\
		(void)v;					\
		ptr += 16;					\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

#define STRESS_MEMRATE_READ_RATE(size, type, prefetch)		\
static uint64_t TARGET_CLONES OPTIMIZE3 stress_memrate_read_rate##size(		\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	register type *ptr;					\
	type v;							\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
	const uint64_t loops = 					\
		stress_memrate_loops(context, sizeof(type) * 16);\
	const uint64_t loop_elements = loops * 16;		\
	uint64_t loop_size = loops * sizeof(type) * 16;		\
	double t1, total_dur = 0.0;				\
	const double dur = (double)loop_size / 			\
		(MB * (double)context->memrate_rd_mbs);		\
								\
	t1 = stress_time_now();					\
	for (ptr = start; ptr < end;) {				\
		double t2, dur_remainder;			\
		const type *loop_end = ptr + loop_elements;	\
		register type *read_end = (type *)		\
			STRESS_PTR_MINIMUM(loop_end, end);	\
								\
		while (ptr < read_end) {			\
			prefetch((uint8_t *)ptr + STRESS_MEMRATE_PF_OFFSET, 0, 3);	\
			v = *(volatile type *)&ptr[0];		\
			(void)v;				\
			v = *(volatile type *)&ptr[1];		\
			(void)v;				\
			v = *(volatile type *)&ptr[2];		\
			(void)v;				\
			v = *(volatile type *)&ptr[3];		\
			(void)v;				\
			v = *(volatile type *)&ptr[4];		\
			(void)v;				\
			v = *(volatile type *)&ptr[5];		\
			(void)v;				\
			v = *(volatile type *)&ptr[6];		\
			(void)v;				\
			v = *(volatile type *)&ptr[7];		\
			(void)v;				\
			v = *(volatile type *)&ptr[8];		\
			(void)v;				\
			v = *(volatile type *)&ptr[9];		\
			(void)v;				\
			v = *(volatile type *)&ptr[10];		\
			(void)v;				\
			v = *(volatile type *)&ptr[11];		\
			(void)v;				\
			v = *(volatile type *)&ptr[12];		\
			(void)v;				\
			v = *(volatile type *)&ptr[13];		\
			(void)v;				\
			v = *(volatile type *)&ptr[14];		\
			(void)v;				\
			v = *(volatile type *)&ptr[15];		\
			(void)v;				\
			ptr += 16;				\
		}						\
		t2 = stress_time_now();				\
		total_dur += dur;				\
		dur_remainder = total_dur - (t2 - t1);		\
								\
		if (dur_remainder >= 0.0) {			\
			struct timespec t;			\
			time_t sec = (time_t)dur_remainder;	\
								\
			t.tv_sec = sec;				\
			t.tv_nsec = (long)((dur_remainder -	\
				(double)sec) *			\
				STRESS_NANOSECOND);		\
			(void)nanosleep(&t, NULL);		\
		}						\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

#define no_prefetch(ptr, arg1, arg2)

#if defined(HAVE_VECMATH)
STRESS_MEMRATE_READ(1024, stress_uint32w1024_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(1024, stress_uint32w1024_t, no_prefetch)
STRESS_MEMRATE_READ(512, stress_uint32w512_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(512, stress_uint32w512_t, no_prefetch)
STRESS_MEMRATE_READ(256, stress_uint32w256_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(256, stress_uint32w256_t, no_prefetch)
STRESS_MEMRATE_READ(128, stress_uint32w128_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(128, stress_uint32w128_t, no_prefetch)
#endif
#if defined(HAVE_INT128_T) && !defined(HAVE_VECMATH)
STRESS_MEMRATE_READ(128, __uint128_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(128, __uint128_t, no_prefetch)
#endif

STRESS_MEMRATE_READ(64, uint64_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(64, uint64_t, no_prefetch)
STRESS_MEMRATE_READ(32, uint32_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(32, uint32_t, no_prefetch)
STRESS_MEMRATE_READ(16, uint16_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(16, uint16_t, no_prefetch)
STRESS_MEMRATE_READ(8, uint8_t, no_prefetch)
STRESS_MEMRATE_READ_RATE(8, uint8_t, no_prefetch)

#if defined(HAVE_BUILTIN_PREFETCH)
#if defined(HAVE_INT128_T)
STRESS_MEMRATE_READ(128pf, __uint128_t, shim_builtin_prefetch)
STRESS_MEMRATE_READ_RATE(128pf, __uint128_t, shim_builtin_prefetch)
#endif
STRESS_MEMRATE_READ(64pf, uint64_t, shim_builtin_prefetch)
STRESS_MEMRATE_READ_RATE(64pf, uint64_t, shim_builtin_prefetch)
#endif

static uint64_t stress_memrate_memset(
	const stress_memrate_context_t *context,
	bool *valid)
{
	const size_t size = context->memrate_bytes;

	(void)shim_memset(context->start, 0xaa, size);

	*valid = true;
	return (uint64_t)size / KB;
}

static uint64_t OPTIMIZE3 stress_memrate_memset_rate(
	const stress_memrate_context_t *context,
	bool *valid)
{
	uint8_t *start ALIGNED(4096) = (uint8_t *)context->start;
	uint8_t *end ALIGNED(4096) = (uint8_t *)context->end;
	const size_t size = end - start;
	const size_t chunk_size = (size > MB) ? MB : size;
	register uint8_t *ptr;
	double t1, t2, total_dur = 0.0, dur_remainder;
	const double dur = (double)chunk_size / (MB * (double)context->memrate_wr_mbs);

	t1 = stress_time_now();
	for (ptr = start; (ptr + chunk_size) < end; ptr += chunk_size) {
		(void)shim_memset(ptr, 0xaa, chunk_size);

		t2 = stress_time_now();
		total_dur += dur;
		dur_remainder = total_dur - (t2 - t1);

		if (dur_remainder >= 0.0) {
			struct timespec t;
			time_t sec = (time_t)dur_remainder;

			t.tv_sec = sec;
			t.tv_nsec = (long)((dur_remainder -
				(double)sec) *
				STRESS_NANOSECOND);
			(void)nanosleep(&t, NULL);
		}
	}

	if (end - ptr > 0) {
		(void)shim_memset(ptr, 0xaa, end - ptr);
		t2 = stress_time_now();
		total_dur += dur;
		dur_remainder = total_dur - (t2 - t1);

		if (dur_remainder >= 0.0) {
			struct timespec t;
			time_t sec = (time_t)dur_remainder;

			t.tv_sec = sec;
			t.tv_nsec = (long)((dur_remainder -
				(double)sec) *
				STRESS_NANOSECOND);
			(void)nanosleep(&t, NULL);
		}
		ptr = end;
	}

	*valid = true;
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;
}

#define STRESS_MEMRATE_WRITE(size, type)			\
static uint64_t TARGET_CLONES OPTIMIZE3	stress_memrate_write##size(	\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
	register type v, *ptr;					\
								\
	{							\
		type vaa;					\
								\
		(void)shim_memset(&vaa, 0xaa, sizeof(vaa));	\
		v = vaa;					\
	}							\
								\
	for (ptr = start; ptr < end; ptr += 16) {		\
		ptr[0] = v;					\
		ptr[1] = v;					\
		ptr[2] = v;					\
		ptr[3] = v;					\
		ptr[4] = v;					\
		ptr[5] = v;					\
		ptr[6] = v;					\
		ptr[7] = v;					\
		ptr[8] = v;					\
		ptr[9] = v;					\
		ptr[10] = v;					\
		ptr[11] = v;					\
		ptr[12] = v;					\
		ptr[13] = v;					\
		ptr[14] = v;					\
		ptr[15] = v;					\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

#if (defined(HAVE_ASM_X86_REP_STOSQ) ||		\
     defined(HAVE_ASM_X86_REP_STOSD) ||		\
     defined(HAVE_ASM_X86_REP_STOSW) ||		\
     defined(HAVE_ASM_X86_REP_STOSB)) &&	\
    !defined(__ILP32__)
static inline uint64_t OPTIMIZE3 stress_memrate_stos(
	const stress_memrate_context_t *context,
	bool *valid,
	void (*func)(void *ptr, const uint32_t loops),
	const size_t wr_size)
{
	uint8_t *start ALIGNED(4096) = (uint8_t *)context->start;
	uint8_t *end ALIGNED(4096) = (uint8_t *)context->end;
	const size_t size = end - start;
	const size_t chunk_size = (size > MB) ? MB : size;
	uint32_t loops = (uint32_t)(chunk_size / wr_size);
	register uint8_t *ptr;

	for (ptr = start; (ptr + chunk_size) < end; ptr += chunk_size) {
		func((void *)ptr, loops);
	}
	/* And any residual less than chunk_size .. */
	loops = (uint32_t)((end - ptr)/ wr_size);
	if (loops) {
		func((void *)ptr, loops);
		ptr = end;
	}

	*valid = true;
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;
}

static inline uint64_t OPTIMIZE3 stress_memrate_stos_rate(
	const stress_memrate_context_t *context,
	bool *valid,
	void (*func)(void *ptr, const uint32_t loops),
	const size_t wr_size)
{
	uint8_t *start ALIGNED(4096) = (uint8_t *)context->start;
	uint8_t *end ALIGNED(4096) = (uint8_t *)context->end;
	const size_t size = end - start;
	const size_t chunk_size = (size > MB) ? MB : size;
	uint32_t loops = (uint32_t)(chunk_size / wr_size);
	register uint8_t *ptr;
	double t1, t2, total_dur = 0.0, dur_remainder;
	const double dur = (double)chunk_size / (MB * (double)context->memrate_wr_mbs);

	t1 = stress_time_now();
	for (ptr = start; (ptr + chunk_size) < end; ptr += chunk_size) {
		func((void *)ptr, loops);

		t2 = stress_time_now();
		total_dur += dur;
		dur_remainder = total_dur - (t2 - t1);

		if (dur_remainder >= 0.0) {
			struct timespec t;
			time_t sec = (time_t)dur_remainder;

			t.tv_sec = sec;
			t.tv_nsec = (long)((dur_remainder -
				(double)sec) *
				STRESS_NANOSECOND);
			(void)nanosleep(&t, NULL);
		}
	}

	/* And any residual less than chunk_size .. */
	loops = (uint32_t)((end - ptr)/ wr_size);
	if (loops) {
		func((void *)ptr, loops);
		t2 = stress_time_now();
		total_dur += dur;
		dur_remainder = total_dur - (t2 - t1);

		if (dur_remainder >= 0.0) {
			struct timespec t;
			time_t sec = (time_t)dur_remainder;

			t.tv_sec = sec;
			t.tv_nsec = (long)((dur_remainder -
				(double)sec) *
				STRESS_NANOSECOND);
			(void)nanosleep(&t, NULL);
		}
		ptr = end;
	}

	*valid = true;
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;
}
#endif

#if defined(HAVE_ASM_X86_REP_STOSQ) &&	\
    !defined(__ILP32__)
static inline void OPTIMIZE3 stress_memrate_stosq(void *ptr, const uint32_t loops)
{
	register void *p = ptr;
	register const uint32_t l = loops;

	__asm__ __volatile__(
		"mov $0xaaaaaaaaaaaaaaaa,%%rax\n;"
		"mov %0,%%rdi\n;"
		"mov %1,%%ecx\n;"
		"rep stosq %%rax,%%es:(%%rdi);\n"
		:
		: "r" (p),
		  "r" (l)
		: "ecx","rdi","rax");
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos64(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos(context, valid, stress_memrate_stosq, sizeof(uint64_t));
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos_rate64(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos_rate(context, valid, stress_memrate_stosq, sizeof(uint64_t));
}
#endif

#if defined(HAVE_ASM_X86_REP_STOSD) &&	\
    !defined(__ILP32__)
static inline void OPTIMIZE3 stress_memrate_stosd(void *ptr, const uint32_t loops)
{
	register void *p = ptr;
	register const uint32_t l = loops;

	__asm__ __volatile__(
		"mov $0xaaaaaaaa,%%eax\n;"
		"mov %0,%%rdi\n;"
		"mov %1,%%ecx\n;"
		"rep stosl %%eax,%%es:(%%rdi);\n"	/* gcc calls it stosl and not stosw */
		:
		: "r" (p),
		  "r" (l)
		: "ecx","rdi","eax");
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos32(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos(context, valid, stress_memrate_stosd, sizeof(uint32_t));
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos_rate32(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos_rate(context, valid, stress_memrate_stosd, sizeof(uint32_t));
}
#endif

#if defined(HAVE_ASM_X86_REP_STOSW) &&	\
    !defined(__ILP32__)
static inline void OPTIMIZE3 stress_memrate_stosw(void *ptr, const uint32_t loops)
{
	register void *p = ptr;
	register const uint32_t l = loops;

	__asm__ __volatile__(
		"mov $0xaaaa,%%ax\n;"
		"mov %0,%%rdi\n;"
		"mov %1,%%ecx\n;"
		"rep stosw %%ax,%%es:(%%rdi);\n"
		:
		: "r" (p),
		  "r" (l)
		: "ecx","rdi","ax");
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos16(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos(context, valid, stress_memrate_stosw, sizeof(uint16_t));
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos_rate16(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos_rate(context, valid, stress_memrate_stosw, sizeof(uint16_t));
}
#endif

#if defined(HAVE_ASM_X86_REP_STOSB) &&	\
    !defined(__ILP32__)
static inline void OPTIMIZE3 stress_memrate_stosb(void *ptr, const uint32_t loops)
{
	register void *p = ptr;
	register const uint32_t l = loops;

	__asm__ __volatile__(
		"mov $0xaa,%%al\n;"
		"mov %0,%%rdi\n;"
		"mov %1,%%ecx\n;"
		"rep stosb %%al,%%es:(%%rdi);\n"
		:
		: "r" (p),
		  "r" (l)
		: "ecx","rdi","al");
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos8(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos(context, valid, stress_memrate_stosb, sizeof(uint8_t));
}

static inline uint64_t OPTIMIZE3 stress_memrate_write_stos_rate8(
        const stress_memrate_context_t *context,
        bool *valid)
{
	return stress_memrate_stos_rate(context, valid, stress_memrate_stosb, sizeof(uint8_t));
}
#endif

#define STRESS_MEMRATE_WRITE_RATE(size, type)			\
static uint64_t TARGET_CLONES OPTIMIZE3 stress_memrate_write_rate##size(	\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
	const uint64_t loops = 					\
		stress_memrate_loops(context, sizeof(type) * 16);\
	uint64_t loop_size = loops * sizeof(type) * 16;		\
	const uint64_t loop_elements = loops * 16;		\
	double t1, total_dur = 0.0;				\
	const double dur = (double)loop_size / 			\
		(MB * (double)context->memrate_wr_mbs);		\
	register type v, *ptr;					\
								\
	{							\
		type vaa;					\
								\
		(void)shim_memset(&vaa, 0xaa, sizeof(vaa));	\
		v = vaa;					\
	}							\
								\
	t1 = stress_time_now();					\
	for (ptr = start; ptr < end;) {				\
		double t2, dur_remainder;			\
		const type *loop_end = ptr + loop_elements;	\
		register type *write_end = (type *)		\
			STRESS_PTR_MINIMUM(loop_end, end);	\
								\
		while (ptr < write_end) {			\
			ptr[0] = v;				\
			ptr[1] = v;				\
			ptr[2] = v;				\
			ptr[3] = v;				\
			ptr[4] = v;				\
			ptr[5] = v;				\
			ptr[6] = v;				\
			ptr[7] = v;				\
			ptr[8] = v;				\
			ptr[9] = v;				\
			ptr[10] = v;				\
			ptr[11] = v;				\
			ptr[12] = v;				\
			ptr[13] = v;				\
			ptr[14] = v;				\
			ptr[15] = v;				\
			ptr += 16;				\
		}						\
		t2 = stress_time_now();				\
		total_dur += dur;				\
		dur_remainder = total_dur - (t2 - t1);		\
								\
		if (dur_remainder >= 0.0) {			\
			struct timespec t;			\
			time_t sec = (time_t)dur_remainder;	\
								\
			t.tv_sec = sec;				\
			t.tv_nsec = (long)((dur_remainder -	\
				(double)sec) * 			\
				STRESS_NANOSECOND);		\
			(void)nanosleep(&t, NULL);		\
		}						\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

/*
 *
 * See https://akkadia.org/drepper/cpumemory.pdf - section 6.1
 *  non-temporal writes using movntdq. Data is not going to be
 *  read, so no need to cache. Write directly to memory.
 */
#define STRESS_MEMRATE_WRITE_NT(size, type, op, init)		\
static uint64_t OPTIMIZE3 stress_memrate_write_nt##size(	\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
	register type v, *ptr;					\
								\
	if (!stress_cpu_x86_has_sse2()) {			\
		*valid = false;					\
		return 0;					\
	}							\
								\
	{							\
		type vaa;					\
								\
		(void)shim_memset(&vaa, 0xaa, sizeof(vaa));	\
		v = vaa;					\
	}							\
								\
	for (ptr = start; ptr < end;) {				\
		register type *vptr = (type *)ptr;		\
								\
		ptr += 16;					\
		op(vptr + 0, v);				\
		op(vptr + 1, v);				\
		op(vptr + 2, v);				\
		op(vptr + 3, v);				\
		op(vptr + 4, v);				\
		op(vptr + 5, v);				\
		op(vptr + 6, v);				\
		op(vptr + 7, v);				\
		op(vptr + 8, v);				\
		op(vptr + 9, v);				\
		op(vptr + 10, v);				\
		op(vptr + 11, v);				\
		op(vptr + 12, v);				\
		op(vptr + 13, v);				\
		op(vptr + 14, v);				\
		op(vptr + 15, v);				\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

#define STRESS_MEMRATE_WRITE_NT_RATE(size, type, op, init)	\
static uint64_t OPTIMIZE3 stress_memrate_write_nt_rate##size(	\
	const stress_memrate_context_t *context,		\
	bool *valid)						\
{								\
	void *start ALIGNED(4096) = context->start;		\
	const type *end ALIGNED(4096) = (type *)context->end;	\
	const uint64_t loops = 					\
		stress_memrate_loops(context, sizeof(type) * 16);\
	uint64_t loop_size = loops * sizeof(type) * 16;		\
	const uint64_t loop_elements = loops * 16;		\
	double t1, total_dur = 0.0;				\
	const double dur = (double)loop_size / 			\
		(MB * (double)context->memrate_wr_mbs);		\
	register type v, *ptr;					\
								\
	if (!stress_cpu_x86_has_sse2()) {			\
		*valid = false;					\
		return 0;					\
	}							\
								\
	{							\
		type vaa;					\
								\
		(void)shim_memset(&vaa, 0xaa, sizeof(vaa));	\
		v = vaa;					\
	}							\
								\
	t1 = stress_time_now();					\
	for (ptr = start; ptr < end;) {				\
		double t2, dur_remainder;			\
		const type *loop_end = ptr + loop_elements;	\
		register type *write_end = (type *)		\
			STRESS_PTR_MINIMUM(loop_end, end);	\
								\
		while (ptr < write_end) {			\
			register type *vptr = (type *)ptr;	\
								\
			ptr += 16;				\
			op(vptr + 0, v);			\
			op(vptr + 1, v);			\
			op(vptr + 2, v);			\
			op(vptr + 3, v);			\
			op(vptr + 4, v);			\
			op(vptr + 5, v);			\
			op(vptr + 6, v);			\
			op(vptr + 7, v);			\
			op(vptr + 8, v);			\
			op(vptr + 9, v);			\
			op(vptr + 10, v);			\
			op(vptr + 11, v);			\
			op(vptr + 12, v);			\
			op(vptr + 13, v);			\
			op(vptr + 14, v);			\
			op(vptr + 15, v);			\
		}						\
		t2 = stress_time_now();				\
		total_dur += dur;				\
		dur_remainder = total_dur - (t2 - t1);		\
								\
		if (dur_remainder >= 0.0) {			\
			struct timespec t;			\
			time_t sec = (time_t)dur_remainder;	\
								\
			t.tv_sec = sec;				\
			t.tv_nsec = (long)((dur_remainder -	\
				(double)sec) * 			\
				STRESS_NANOSECOND);		\
			(void)nanosleep(&t, NULL);		\
		}						\
	}							\
	*valid = true;						\
	return ((uintptr_t)ptr - (uintptr_t)start) / KB;	\
}

#if defined(HAVE_NT_STORE128)
STRESS_MEMRATE_WRITE_NT(128, __uint128_t, stress_nt_store128, i)
STRESS_MEMRATE_WRITE_NT_RATE(128, __uint128_t, stress_nt_store128, i)
#endif

#if defined(HAVE_NT_STORE64)
STRESS_MEMRATE_WRITE_NT(64, uint64_t, stress_nt_store64, i)
STRESS_MEMRATE_WRITE_NT_RATE(64, uint64_t, stress_nt_store64, i)
#endif

#if defined(HAVE_NT_STORE32)
STRESS_MEMRATE_WRITE_NT(32, uint32_t, stress_nt_store32, i)
STRESS_MEMRATE_WRITE_NT_RATE(32, uint32_t, stress_nt_store32, i)
#endif

#if defined(HAVE_VECMATH)
STRESS_MEMRATE_WRITE(1024, stress_uint32w1024_t)
STRESS_MEMRATE_WRITE_RATE(1024, stress_uint32w1024_t)
STRESS_MEMRATE_WRITE(512, stress_uint32w512_t)
STRESS_MEMRATE_WRITE_RATE(512, stress_uint32w512_t)
STRESS_MEMRATE_WRITE(256, stress_uint32w256_t)
STRESS_MEMRATE_WRITE_RATE(256, stress_uint32w256_t)
STRESS_MEMRATE_WRITE(128, stress_uint32w128_t)
STRESS_MEMRATE_WRITE_RATE(128, stress_uint32w128_t)
#endif
#if defined(HAVE_INT128_T) && !defined(HAVE_VECMATH)
STRESS_MEMRATE_WRITE(128, __uint128_t)
STRESS_MEMRATE_WRITE_RATE(128, __uint128_t)
#endif
STRESS_MEMRATE_WRITE(64, uint64_t)
STRESS_MEMRATE_WRITE_RATE(64, uint64_t)
STRESS_MEMRATE_WRITE(32, uint32_t)
STRESS_MEMRATE_WRITE_RATE(32, uint32_t)
STRESS_MEMRATE_WRITE(16, uint16_t)
STRESS_MEMRATE_WRITE_RATE(16, uint16_t)
STRESS_MEMRATE_WRITE(8, uint8_t)
STRESS_MEMRATE_WRITE_RATE(8, uint8_t)

static stress_memrate_info_t memrate_info[] = {
#if defined(HAVE_ASM_X86_REP_STOSQ) &&	\
    !defined(__ILP32__)
	{ "write64stoq", MR_WR,	stress_memrate_write_stos64,	stress_memrate_write_stos_rate64 },
#endif
#if defined(HAVE_ASM_X86_REP_STOSD) &&	\
    !defined(__ILP32__)
	{ "write32stow",MR_WR,	stress_memrate_write_stos32,	stress_memrate_write_stos_rate32 },
#endif
#if defined(HAVE_ASM_X86_REP_STOSW) &&	\
    !defined(__ILP32__)
	{ "write16stod",MR_WR,	stress_memrate_write_stos16,	stress_memrate_write_stos_rate16 },
#endif
#if defined(HAVE_ASM_X86_REP_STOSB) &&	\
    !defined(__ILP32__)
	{ "write8stob",	MR_WR,	stress_memrate_write_stos8,	stress_memrate_write_stos_rate8 },
#endif
#if defined(HAVE_NT_STORE128)
	{ "write128nt",	MR_WR, stress_memrate_write_nt128,	stress_memrate_write_nt_rate128 },
#endif
#if defined(HAVE_NT_STORE64)
	{ "write64nt",	MR_WR, stress_memrate_write_nt64,	stress_memrate_write_nt_rate64 },
#endif
#if defined(HAVE_NT_STORE32)
	{ "write32nt",	MR_WR, stress_memrate_write_nt32,	stress_memrate_write_nt_rate32 },
#endif
#if defined(HAVE_VECMATH)
	{ "write1024",	MR_WR, stress_memrate_write1024,	stress_memrate_write_rate1024 },
	{ "write512",	MR_WR, stress_memrate_write512,		stress_memrate_write_rate512 },
	{ "write256",	MR_WR, stress_memrate_write256,		stress_memrate_write_rate256 },
	{ "write128",	MR_WR, stress_memrate_write128,		stress_memrate_write_rate128 },
#endif
#if defined(HAVE_INT128_T) && !defined(HAVE_VECMATH)
	{ "write128",	MR_WR, stress_memrate_write128,		stress_memrate_write_rate128 },
#endif
	{ "write64",	MR_WR, stress_memrate_write64,		stress_memrate_write_rate64 },
	{ "write32",	MR_WR, stress_memrate_write32,		stress_memrate_write_rate32 },
	{ "write16",	MR_WR, stress_memrate_write16,		stress_memrate_write_rate16 },
	{ "write8",	MR_WR, stress_memrate_write8,		stress_memrate_write_rate8 },
	{ "memset",	MR_WR, stress_memrate_memset,		stress_memrate_memset_rate },
#if defined(HAVE_BUILTIN_PREFETCH)
#if defined(HAVE_INT128_T)
	{ "read128pf",	MR_RD, stress_memrate_read128pf,	stress_memrate_read_rate128pf },
#endif
	{ "read64pf",	MR_RD, stress_memrate_read64pf,		stress_memrate_read_rate64pf },
#endif
#if defined(HAVE_VECMATH)
	{ "read1024",	MR_RD, stress_memrate_read1024,		stress_memrate_read_rate1024 },
	{ "read512",	MR_RD, stress_memrate_read512,		stress_memrate_read_rate512 },
	{ "read256",	MR_RD, stress_memrate_read256,		stress_memrate_read_rate256 },
	{ "read128",	MR_RD, stress_memrate_read128,		stress_memrate_read_rate128 },
#endif
#if defined(HAVE_INT128_T) && !defined(HAVE_VECMATH)
	{ "read128",	MR_RD, stress_memrate_read128,		stress_memrate_read_rate128 },
#endif
	{ "read64",	MR_RD, stress_memrate_read64,		stress_memrate_read_rate64 },
	{ "read32",	MR_RD, stress_memrate_read32,		stress_memrate_read_rate32 },
	{ "read16",	MR_RD, stress_memrate_read16,		stress_memrate_read_rate16 },
	{ "read8",	MR_RD, stress_memrate_read8,		stress_memrate_read_rate8 },
};

static const size_t memrate_items = SIZEOF_ARRAY(memrate_info);

static void OPTIMIZE3 stress_memrate_init_data(
	void *start,
	void *end)
{
	register volatile uint32_t *ptr;

	for (ptr = start; ptr < (uint32_t *)end; ptr++)
		*ptr = stress_mwc32();
}

static inline void *stress_memrate_mmap(const stress_args_t *args, uint64_t sz)
{
	void *ptr;

	ptr = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE,
#if defined(MAP_POPULATE)
		MAP_POPULATE |
#endif
#if defined(HAVE_MADVISE)
		MAP_PRIVATE |
#else
		MAP_SHARED |
#endif
		MAP_ANONYMOUS, -1, 0);
	/* Coverity Scan believes NULL can be returned, doh */
	if (!ptr || (ptr == MAP_FAILED)) {
		pr_err("%s: cannot allocate %" PRIu64 " K\n",
			args->name, sz / 1024);
		ptr = MAP_FAILED;
	} else {
#if defined(HAVE_MADVISE) &&	\
    defined(MADV_HUGEPAGE)

		VOID_RET(int, madvise(ptr, sz, MADV_HUGEPAGE));
#endif
	}
	return ptr;
}

static inline uint64_t stress_memrate_dispatch(
	const stress_memrate_info_t *info,
	const stress_memrate_context_t *context,
	bool *valid)
{
	if (((info->rdwr == MR_RD) && (context->memrate_rd_mbs == ~0ULL)) ||
	    ((info->rdwr == MR_WR) && (context->memrate_wr_mbs == ~0ULL))) {
		return info->func(context, valid);
	}
	return info->func_rate(context, valid);
}

static int stress_memrate_child(const stress_args_t *args, void *ctxt)
{
	stress_memrate_context_t *context = (stress_memrate_context_t *)ctxt;
	void *buffer, *buffer_end;

	buffer = stress_memrate_mmap(args, context->memrate_bytes);
	if (buffer == MAP_FAILED)
		return EXIT_NO_RESOURCE;

	buffer_end = (uint8_t *)buffer + context->memrate_bytes;
	stress_memrate_init_data(buffer, buffer_end);

	context->start = buffer;
	context->end = buffer_end;

	if (sigsetjmp(jmpbuf, 1) != 0)
		goto tidy;

	if (stress_sighandler(args->name, SIGALRM, stress_memrate_alarm_handler, NULL) < 0)
		return EXIT_NO_RESOURCE;

	do {
		size_t i;

		for (i = 0; i < memrate_items; i++) {
			double t1, t2;
			uint64_t kbytes;
			stress_memrate_info_t *info = &memrate_info[i];
			bool valid = false;

			if (context->memrate_flush)
				stress_memrate_flush(context);
			t1 = stress_time_now();
			kbytes = stress_memrate_dispatch(info, context, &valid);
			context->stats[i].kbytes += (double)kbytes;
			t2 = stress_time_now();
			context->stats[i].duration += (t2 - t1);
			context->stats[i].valid = valid;

			if (!keep_stressing(args))
				break;
		}
		inc_counter(args);
	} while (keep_stressing(args));

tidy:
	(void)munmap((void *)buffer, context->memrate_bytes);
	return EXIT_SUCCESS;
}

/*
 *  stress_memrate()
 *	stress cache/memory/CPU with memrate stressors
 */
static int stress_memrate(const stress_args_t *args)
{
	int rc;
	size_t i, stats_size;
	stress_memrate_context_t context;

	context.memrate_bytes = DEFAULT_MEMRATE_BYTES;
	context.memrate_rd_mbs = ~0ULL;
	context.memrate_wr_mbs = ~0ULL;
	context.memrate_flush = false;

	(void)stress_get_setting("memrate-bytes", &context.memrate_bytes);
	(void)stress_get_setting("memrate-flush", &context.memrate_flush);
	(void)stress_get_setting("memrate-rd-mbs", &context.memrate_rd_mbs);
	(void)stress_get_setting("memrate-wr-mbs", &context.memrate_wr_mbs);

	stats_size = memrate_items * sizeof(*context.stats);
	stats_size = (stats_size + args->page_size - 1) & ~(args->page_size - 1);

	context.stats = (stress_memrate_stats_t *)mmap(NULL, stats_size,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (context.stats == MAP_FAILED) {
		pr_inf_skip("%s: failed to mmap %zd byte statistics buffer, skipping stressor\n",
			args->name, stats_size);
		return EXIT_NO_RESOURCE;
	}
	for (i = 0; i < memrate_items; i++) {
		context.stats[i].duration = 0.0;
		context.stats[i].kbytes = 0.0;
		context.stats[i].valid = false;
	}

	context.memrate_bytes = (context.memrate_bytes + 1023) & ~(1023ULL);
	if (args->instance == 0) {
		pr_inf("%s: using buffer size of %" PRIu64 "K, cache flushing %s\n", args->name,
			context.memrate_bytes >> 10,
			context.memrate_flush ? "enabled" : "disabled");
		if ((context.memrate_bytes > MB) && (context.memrate_bytes & MB)) {
			pr_inf("%s: for optimial speed, use multiples of 1 MB for --memrate-bytes\n", args->name);
		}
		if (!context.memrate_flush)
			pr_inf("%s: cache flushing can be enabled with --memrate-flush option\n", args->name);
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	rc = stress_oomable_child(args, &context, stress_memrate_child, STRESS_OOMABLE_NORMAL);

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	pr_lock();
	for (i = 0; i < memrate_items; i++) {
		if (!context.stats[i].valid)
			continue;
		if (context.stats[i].duration > 0.0) {
			char tmp[32];
			const double rate = context.stats[i].kbytes / (context.stats[i].duration * KB);

			(void)snprintf(tmp, sizeof(tmp), "%s MB per sec", memrate_info[i].name);
			stress_metrics_set(args, i, tmp, rate);
		} else {
			pr_inf("%s: %10.10s: interrupted early\n",
				args->name, memrate_info[i].name);
		}
	}
	pr_unlock();

	(void)munmap((void *)context.stats, stats_size);

	return rc;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_memrate_bytes,	stress_set_memrate_bytes },
	{ OPT_memrate_flush,	stress_set_memrate_flush },
	{ OPT_memrate_rd_mbs,	stress_set_memrate_rd_mbs },
	{ OPT_memrate_wr_mbs,	stress_set_memrate_wr_mbs },
	{ 0,			NULL }
};

stressor_info_t stress_memrate_info = {
	.stressor = stress_memrate,
	.class = CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
