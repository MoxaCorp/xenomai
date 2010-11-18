#include <stdio.h>
#include <stdlib.h>
#include "copperplate/traceobj.h"
#include "copperplate/threadobj.h"
#include "copperplate/heapobj.h"

struct tracemark {
	const char *file;
	int line;
	int mark;
};

void traceobj_init(struct traceobj *trobj, const char *label, int nr_marks)
{
	pthread_mutexattr_t mattr;
	pthread_condattr_t cattr;

	__traceobj_core_init();

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
	pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_PRIVATE);
	pthread_mutex_init(&trobj->lock, &mattr);
	pthread_mutexattr_destroy(&mattr);
	pthread_condattr_init(&cattr);
	pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_PRIVATE);
	pthread_cond_init(&trobj->join, &cattr);
	pthread_condattr_destroy(&cattr);
	trobj->nr_threads = 0;

	trobj->label = label;
	trobj->nr_marks = nr_marks;
	trobj->cur_mark = 0;

	if (nr_marks > 0) {
		trobj->marks = pvmalloc(sizeof(struct tracemark) * nr_marks);
		if (trobj->marks == NULL)
			panic("cannot allocate mark table for tracing");
	}
}

static void compare_marks(struct traceobj *trobj, int tseq[], int nr_seq) /* lock held */
{
	int mark;

	for (mark = 0; mark < trobj->cur_mark || mark < nr_seq; mark++) {
		if (mark >= trobj->cur_mark) {
			fprintf(stderr, " <missing mark> |  [%d] expected\n",
				tseq[mark]);
		} else if (mark < nr_seq)
			fprintf(stderr, "at %s:%d  |  [%d] should be [%d]\n",
				trobj->marks[mark].file,
				trobj->marks[mark].line,
				trobj->marks[mark].mark,
				tseq[mark]);
		else
			fprintf(stderr, "at %s:%d  |  unexpected [%d]\n",
				trobj->marks[mark].file,
				trobj->marks[mark].line,
				trobj->marks[mark].mark);
	}

	fflush(stderr);
}

void traceobj_verify(struct traceobj *trobj, int tseq[], int nr_seq)
{
	int end_mark, mark;

	if (nr_seq > trobj->nr_marks)
		goto fail;

	pthread_mutex_lock(&trobj->lock);

	end_mark = trobj->cur_mark;
	if (end_mark == 0)
		panic("no mark defined");

	if (end_mark != nr_seq)
		goto fail;

	for (mark = 0; mark < end_mark; mark++) {
		if (trobj->marks[mark].mark != tseq[mark])
			goto fail;
	}

	pthread_mutex_unlock(&trobj->lock);
	return;

fail:
	warning("mismatching execution sequence detected");
	compare_marks(trobj, tseq, nr_seq);
	exit(5);
}

void traceobj_destroy(struct traceobj *trobj)
{
	pvfree(trobj->marks);
	pthread_mutex_destroy(&trobj->lock);
}

static void dump_marks(struct traceobj *trobj) /* lock held */
{
	int mark;

	for (mark = 0; mark < trobj->cur_mark; mark++)
		fprintf(stderr, "[%d] at %s:%d\n",
			trobj->marks[mark].mark,
			trobj->marks[mark].file,
			trobj->marks[mark].line);

	fflush(stderr);
}

void __traceobj_assert_failed(struct traceobj *trobj,
			      const char *file, int line, const char *cond)
{
	pthread_mutex_lock(&trobj->lock);
	dump_marks(trobj);
	pthread_mutex_unlock(&trobj->lock);
	panic("trace assertion failed:\n%s:%d => \"%s\"", file, line, cond);
}

void __traceobj_mark(struct traceobj *trobj,
		     const char *file, int line, int mark)
{
	struct tracemark *tmk;
	int cur_mark;

	pthread_mutex_lock(&trobj->lock);

	cur_mark = trobj->cur_mark++;
	if (cur_mark >= trobj->nr_marks) {
		trobj->cur_mark = cur_mark;
		dump_marks(trobj);
		panic("too many marks");
	}

	tmk = trobj->marks + cur_mark;
	tmk->file = file;
	tmk->line = line;
	tmk->mark = mark;

	pthread_mutex_unlock(&trobj->lock);
}

void traceobj_enter(struct traceobj *trobj)
{
	struct threadobj *current = threadobj_current();

	if (current) {
		__traceobj_core_enter(current);
		threadobj_lock(current);
		current->tracer = trobj;
		threadobj_unlock(current);
	}

	pthread_mutex_lock(&trobj->lock);
	++trobj->nr_threads;
	pthread_mutex_unlock(&trobj->lock);
}

/* May be directly called from finalizer. */
void traceobj_unwind(struct traceobj *trobj)
{
	pthread_mutex_lock(&trobj->lock);
	pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock,
			     &trobj->lock);

	if (--trobj->nr_threads <= 0)
		pthread_cond_signal(&trobj->join);

	pthread_cleanup_pop(1);
}

void traceobj_exit(struct traceobj *trobj)
{
	struct threadobj *current = threadobj_current();

	if (current) {
		__traceobj_core_exit(current);
		threadobj_lock(current);
		current->tracer = NULL;
		threadobj_unlock(current);
	}

	traceobj_unwind(trobj);
}

void traceobj_join(struct traceobj *trobj)
{
	pthread_mutex_lock(&trobj->lock);
	pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock,
			     &trobj->lock);

	while (trobj->nr_threads > 0)
		pthread_cond_wait(&trobj->join, &trobj->lock);

	pthread_cleanup_pop(1);
}
