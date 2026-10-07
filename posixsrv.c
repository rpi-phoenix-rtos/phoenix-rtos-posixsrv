/*
 * Phoenix-RTOS
 *
 * libphoenix
 *
 * POSIX server - implementation
 *
 * Copyright 2018, 2023 Phoenix Systems
 * Author: Jan Sikorski, Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/msg.h>
#include <sys/file.h>
#include <sys/threads.h>
#include <pthread.h>
#include <sys/list.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "posix/idtree.h"

#include "posixsrv_private.h"

#if 0
#define TRACE(str, ...) printf("posixsrv: " str "\n", ##__VA_ARGS__)
#else
#define TRACE(str, ...)
#endif


struct {
	unsigned port;

	pthread_mutex_t lock; /* user-space lock: no system call when free */
	pthread_cond_t cond;  /* CLOCK_MONOTONIC: request wakeups are gettime() times */
	idtree_t objects;
	rbtree_t timeout;

	struct {
		int id;
		object_t *o;
	} cache;
} posixsrv_common;


static void fail(const char *str)
{
	printf("posixsrv fail: %s\n", str);
}


void posixsrv_object_destroy(object_t *o)
{
	o->destroy = 1;
}


object_t *posixsrv_object_get(int id)
{
	object_t *o;

	(void)pthread_mutex_lock(&posixsrv_common.lock);

	if (posixsrv_common.cache.id == id)
		o = posixsrv_common.cache.o;
	else
		o = lib_treeof(object_t, linkage, (void *)idtree_find(&posixsrv_common.objects, id));

	if (o != NULL) {
		if (o->destroy) {
			o = NULL;
		}
		else {
			posixsrv_common.cache.id = id;
			posixsrv_common.cache.o = o;
			o->refs++;
		}
	}

	(void)pthread_mutex_unlock(&posixsrv_common.lock);

	return o;
}


void posixsrv_object_ref(object_t *o)
{
	(void)pthread_mutex_lock(&posixsrv_common.lock);
	o->refs++;
	(void)pthread_mutex_unlock(&posixsrv_common.lock);
}


void posixsrv_object_put(object_t *o)
{
	(void)pthread_mutex_lock(&posixsrv_common.lock);

	if (!--o->refs && o->destroy) {
		TRACE("removing %d", o->linkage.id);

		if (posixsrv_common.cache.id == o->linkage.id)
			posixsrv_common.cache.o = NULL;

		idtree_remove(&posixsrv_common.objects, &o->linkage);
		(void)pthread_mutex_unlock(&posixsrv_common.lock);

		if (o->operations->release != NULL)
			o->operations->release(o);

		return;
	}

	(void)pthread_mutex_unlock(&posixsrv_common.lock);
	return;
}


int posixsrv_object_create(object_t *o, const operations_t *ops)
{
	o->destroy = 0;
	o->operations = ops;
	o->refs = 1;

	(void)pthread_mutex_lock(&posixsrv_common.lock);
	idtree_alloc(&posixsrv_common.objects, &o->linkage);
	posixsrv_common.cache.id = o->linkage.id;
	posixsrv_common.cache.o = o;
	(void)pthread_mutex_unlock(&posixsrv_common.lock);

	TRACE("created %d", o->linkage.id);

	return EOK;
}


int posixsrv_object_link(object_t *o, const char *path)
{
	TRACE("linking %d to %s", o->linkage.id, path);
	oid_t oid;

	oid.port = posixsrv_common.port;
	oid.id = o->linkage.id;

	return create_dev(&oid, path);
}


unsigned posixsrv_port(void)
{
	return posixsrv_common.port;
}


/*
 * Tells the kernel that the poll() status of o may have changed, so poll() and
 * select() callers watching it ask again at once instead of after the kernel's
 * timed re-poll (20 ms). Call it after the state the atPollStatus answer reads
 * has been updated. With nobody watching it is a no-op; a kernel without the
 * call answers -EINVAL and its pollers keep the timed re-poll.
 */
void posixsrv_pollNotify(object_t *o)
{
	oid_t oid;

	oid.port = posixsrv_common.port;
	oid.id = posixsrv_object_id(o);

	(void)pollNotify(&oid);
}


/*
 * Orders the timeout tree by wakeup time, earliest first: the timeout thread
 * sleeps until lib_rbMinimum(). Requests due at the same time are ordered by
 * address, as lib_rbInsert() does not insert a node that compares equal to one
 * already in the tree.
 */
static int rq_cmp(rbnode_t *n1, rbnode_t *n2)
{
	request_t *r1, *r2;
	r1 = lib_treeof(request_t, linkage, n1);
	r2 = lib_treeof(request_t, linkage, n2);

	if (r1->wakeup != r2->wakeup)
		return (r1->wakeup > r2->wakeup) ? 1 : -1;
	if (r1 != r2)
		return ((uintptr_t)r1 > (uintptr_t)r2) ? 1 : -1;
	return 0;
}


void rq_timeout(request_t *r, int ms)
{
	gettime(&r->wakeup, NULL);
	r->wakeup += 1000 * ms;

	(void)pthread_mutex_lock(&posixsrv_common.lock);
	lib_rbInsert(&posixsrv_common.timeout, &r->linkage);
	(void)pthread_mutex_unlock(&posixsrv_common.lock);
	(void)pthread_cond_signal(&posixsrv_common.cond);
}


void rq_setResponse(request_t *r, int response)
{
	switch (r->msg.type) {
	case mtDevCtl:
		ioctl_setResponse(&r->msg, 0, response, NULL);
		break;
	case mtGetAttr:
	case mtSetAttr:
		if (response < 0) {
			r->msg.o.err = response;
			break;
		}
		r->msg.o.attr.val = response;
		r->msg.o.err = EOK;
		break;
	default:
		/* TODO: other cases */
		r->msg.o.err = response;
		break;
	}
}


void rq_wakeup(request_t *r)
{
	TRACE("respond %x", r->rid);
	msgRespond(r->port, &r->msg, r->rid);
	free(r);
}


int rq_id(request_t *r)
{
	int id;
	id_t full_id;

	switch (r->msg.type) {
	case mtOpen:
	case mtClose:
	case mtRead:
	case mtWrite:
	case mtTruncate:
	case mtCreate:
	case mtDestroy:
	case mtSetAttr:
	case mtGetAttr:
	case mtGetAttrAll:
	case mtReaddir:
	case mtLookup:
		id = r->msg.oid.id;
		break;

	case mtLink:
	case mtUnlink:
		id = r->msg.i.ln.oid.id;
		break;

	case mtDevCtl:
		ioctl_unpack(&r->msg, NULL, &full_id);
		id = (int)full_id;
		break;

	default:
		id = -1;
		break;
	}

	return id;
}


void posixsrv_threadMain(void *arg)
{
	object_t *o;
	unsigned port = (uintptr_t)arg;
	request_t *r = NULL;

	for (;;) {
		if (r == NULL) {
			r = malloc(sizeof(*r));
			if (r == NULL) {
				printf("posixsrv: Out of memory\n");
				endthread();
			}
			r->port = port;
		}

		if (msgRecv(port, &r->msg, &r->rid) < 0) {
			continue;
		}

		o = posixsrv_object_get(rq_id(r));

		/* Can't handle msg - wrong object id or wrong operation */
		if (o == NULL || o->operations->handlers[r->msg.type] == NULL) {
			if (o != NULL) {
				posixsrv_object_put(o);
			}
			r->msg.o.err = -EINVAL;
			msgRespond(port, &r->msg, r->rid);
			continue;
		}

		r->object = o;
		r = o->operations->handlers[r->msg.type](o, r);

		/* If an operation returns NULL, it is up to a module to
		 * respond to this msg later and free the request */
		if (r != NULL) {
			msgRespond(port, &r->msg, r->rid);
		}

		posixsrv_object_put(o);
	}
}


void posixsrv_threadRqTimeout(void *arg)
{
	request_t *r;
	time_t now;
	struct timespec deadline;

	(void)pthread_mutex_lock(&posixsrv_common.lock);

	for (;;) {
		r = lib_treeof(request_t, linkage, lib_rbMinimum(posixsrv_common.timeout.root));
		if (r != NULL) {
			gettime(&now, NULL);

			if (r->wakeup <= now) {
				lib_rbRemove(&posixsrv_common.timeout, &r->linkage);
				if (r->object->operations->timeout != NULL) {
					r->object->operations->timeout(r);
				}
				else {
					rq_setResponse(r, -ETIME);
					rq_wakeup(r);
				}
				continue;
			}

			/* gettime() is CLOCK_MONOTONIC, the clock of the condition */
			deadline.tv_sec = r->wakeup / 1000000;
			deadline.tv_nsec = (long)(r->wakeup % 1000000) * 1000;
			(void)pthread_cond_timedwait(&posixsrv_common.cond, &posixsrv_common.lock, &deadline);
		}
		else {
			(void)pthread_cond_wait(&posixsrv_common.cond, &posixsrv_common.lock);
		}
	}
}


int posixsrv_init(unsigned *srvPort, unsigned *eventPort)
{
	idtree_init(&posixsrv_common.objects);
	lib_rbInit(&posixsrv_common.timeout, rq_cmp, NULL);
	pthread_condattr_t cattr;

	if ((pthread_mutex_init(&posixsrv_common.lock, NULL) != 0) ||
			(pthread_condattr_init(&cattr) != 0) ||
			(pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) != 0) ||
			(pthread_cond_init(&posixsrv_common.cond, &cattr) != 0)) {
		fail("lock init");
		return -1;
	}
	(void)pthread_condattr_destroy(&cattr);

	if (portCreate(&posixsrv_common.port) < 0) {
		fail("port create");
		return -1;
	}

	mkdir("/dev", 0777);
	mkdir("/dev/posix", 0777);

	if (special_init() < 0) {
		fail("special init");
		return -1;
	}

	if (event_init(eventPort) < 0) {
		fail("event init");
		return -1;
	}

	if (pipe_init() < 0) {
		fail("pipe init");
		return -1;
	}

	if (pty_init() < 0) {
		fail("pty init");
		return -1;
	}

	if (tmpfile_init() < 0) {
		fail("tmpfile init");
		return -1;
	}

	if (srvPort != NULL) {
		*srvPort = posixsrv_common.port;
	}

	return 0;
}
