/*
 * Phoenix-RTOS
 *
 * POSIX server - semaphores
 *
 * Copyright 2026 Phoenix Systems
 * Author: Michal Lach
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/threads.h>
#include <sys/stat.h>
#include <sys/list.h>
#include <sys/ioctl.h>
#include <sys/semaphore.h>
#include <sys/minmax.h>

#include "posixsrv_private.h"

#if 0
#define SEMAPHORE_TRACE(str, ...) printf("posixsrv semaphore: " str "\n", ##__VA_ARGS__);
#else
#define SEMAPHORE_TRACE(str, ...)
#endif

typedef struct _shared_semaphore_t {
	object_t object;

	char name[NAME_MAX];

	handle_t lock;
	request_t *queue;

	unsigned int value;
} shared_semaphore_t;


_Static_assert(SEM_VALUE_MAX <= UINT_MAX, "The maximum semaphore value SEM_VALUE_MAX shall be less or equal to the maximum possible value of its container");


int semaphore_down(shared_semaphore_t *sem, request_t *request, time_t timeoutms, bool try)
{
	int ret;

	mutexLock(sem->lock);

	if (sem->value > 0) {
		sem->value--;
		ret = EOK;
	}
	else if (try) {
		ret = -EAGAIN;
	}
	else {
		LIST_ADD(&sem->queue, request);
		ret = -EBUSY;
	}

	mutexUnlock(sem->lock);

	if (timeoutms > 0) {
		rq_timeout(request, timeoutms / 1000);
	}

	return ret;
}


int semaphore_up(shared_semaphore_t *sem, request_t *request)
{
	int ret = EOK;
	request_t *last;

	mutexLock(sem->lock);

	if (sem->queue != NULL) {
		last = sem->queue;
		LIST_REMOVE(&sem->queue, last);
		rq_setResponse(last, EOK);
		rq_wakeup(last);
	}
	else if (sem->value + 1 > SEM_VALUE_MAX) {
		ret = -EOVERFLOW;
	}
	else {
		sem->value++;
	}

	mutexUnlock(sem->lock);

	return ret;
}


static request_t *semaphore_open_op(object_t *object, request_t *request)
{
	SEMAPHORE_TRACE("open(%s): refs: %d", ((shared_semaphore_t *)object)->name, object->refs);
	posixsrv_object_ref(object);
	rq_setResponse(request, EOK);

	return request;
}


static request_t *semaphore_close_op(object_t *object, request_t *request)
{
	SEMAPHORE_TRACE("close(%s)", ((shared_semaphore_t *)object)->name);

	posixsrv_object_put(object);
	SEMAPHORE_TRACE("close(%s) refs: %d\n", ((shared_semaphore_t *)object)->name, object->refs);
	rq_setResponse(request, EOK);
	return request;
}


static request_t *semaphore_destroy_op(object_t *object, request_t *request)
{
	shared_semaphore_t *sem = (shared_semaphore_t *)object;

	SEMAPHORE_TRACE("destroy: %s", sem->name);

	posixsrv_object_destroy(object);
	posixsrv_object_put(object);
	unlink(sem->name);

	return request;
}


static void semaphore_release(object_t *object)
{
	shared_semaphore_t *sem = (shared_semaphore_t *)object;
	request_t *cur = NULL;

	SEMAPHORE_TRACE("release(%s)", sem->name);

	/*
	 * This handles a case when semaphore is scheduled for deletion but
	 * still has waiters. Should not happen, but better to define the
	 * behaviour, rather than leaving threads dangling.
	 */
	while (sem->queue != NULL) {
		cur = sem->queue;
		LIST_REMOVE(&sem->queue, cur);
		rq_setResponse(cur, -EINVAL);
		rq_wakeup(cur);
	}

	resourceDestroy(sem->lock);
	free(object);
}


static request_t *semaphore_getattr_op(object_t *object, request_t *request)
{
	shared_semaphore_t *sem = (shared_semaphore_t *)object;

	SEMAPHORE_TRACE("getattr(%s)", sem->name);

	mutexLock(sem->lock);
	request->msg.o.attr.val = sem->value;
	mutexUnlock(sem->lock);

	SEMAPHORE_TRACE("getattr(%s): value %u", sem->name, (unsigned int)request->msg.o.attr.val);

	return request;
}


static request_t *semaphore_devctl_op(object_t *object, request_t *request)
{
	shared_semaphore_t *sem = (shared_semaphore_t *)object;
	unsigned long cmd;
	time_t timeout = -1;
	int ret = EOK;
	const void *in;

	SEMAPHORE_TRACE("devctl(%s)", sem->name);

	in = ioctl_unpack(&request->msg, &cmd, NULL);
	switch (cmd) {
		case SEM_UP:
			SEMAPHORE_TRACE("devctl(%s): UP", sem->name);
			ret = semaphore_up(sem, request);
			break;
		case SEM_DOWN_TRY:
			SEMAPHORE_TRACE("devctl(%s): DOWN_TRY", sem->name);
			ret = semaphore_down(sem, request, 0, true);
			break;
		case SEM_DOWN_TIMEOUT:
			timeout = *(time_t *)in;
			SEMAPHORE_TRACE("devctl(%s): DOWN TIMEOUT %d", sem->name, timeout);
			ret = semaphore_down(sem, request, timeout, false);
			break;
		case SEM_DOWN:
			SEMAPHORE_TRACE("devctl(%s): DOWN", sem->name);
			ret = semaphore_down(sem, request, 0, false);
			break;
		default:
			ret = -EINVAL;
	}

	SEMAPHORE_TRACE("devctl(%s): returns %d", sem->name, ret);

	if (ret == -EBUSY) {
		return NULL; /* blocking on timeout */
	}
	else {
		rq_setResponse(request, ret);
		return request;
	}
}


static void semaphore_timeout(request_t *request)
{
	shared_semaphore_t *sem = (shared_semaphore_t *)request->object;

	SEMAPHORE_TRACE("timeout(%s)", sem->name);

	mutexLock(sem->lock);
	LIST_REMOVE(&sem->queue, request);
	mutexUnlock(sem->lock);

	rq_setResponse(request, -ETIMEDOUT);
	rq_wakeup(request);
}


static operations_t semaphore_ops = {
	.handlers = { NULL },
	.open = semaphore_open_op,
	.close = semaphore_close_op,
	.getattr = semaphore_getattr_op,
	.devctl = semaphore_devctl_op,
	.timeout = semaphore_timeout,
	.destroy = semaphore_destroy_op,
	.release = semaphore_release,
};


int semaphore_create(const char *name, unsigned int value, int mode, int *id)
{
	unsigned int sz;
	shared_semaphore_t *sem;

	if (name == NULL || value > SEM_VALUE_MAX) {
		return -EINVAL;
	}

	if (*name == '/') {
		name++;
	}

	sz = strlen(name);
	if (sz + sizeof(SEMAPHORE_PATH) > PATH_MAX) {
		return -ENAMETOOLONG;
	}

	if (sz == 0 || strchr((char *)name, '/') != NULL) {
		return -EINVAL;
	}

	sem = malloc(sizeof(*sem));
	if (sem == NULL) {
		return -ENOMEM;
	}

	posixsrv_object_create(&sem->object, &semaphore_ops);
	posixsrv_object_put(&sem->object);

	memset(sem->name, 0, sizeof(sem->name));
	strcpy(sem->name, SEMAPHORE_PATH);
	memcpy(sem->name + sizeof(SEMAPHORE_PATH) - 1, name, sz);
	if (posixsrv_object_link(&sem->object, sem->name) < 0) {
		posixsrv_object_destroy(&sem->object);
		posixsrv_object_put(&sem->object);
		free(sem);
		return -EEXIST;
	}

	if (mutexCreate(&sem->lock) < 0) {
		unlink(sem->name);
		posixsrv_object_destroy(&sem->object);
		posixsrv_object_put(&sem->object);
		free(sem);
		return -ENOMEM;
	}

	sem->value = value;
	sem->queue = NULL;

	*id = posixsrv_object_id(&sem->object);

	return EOK;
}


static request_t *semaphore_create_op(object_t *srv, request_t *request)
{
	int id = 0, ret;

	SEMAPHORE_TRACE("create");
	ret = semaphore_create(request->msg.i.data, (unsigned int)request->msg.i.create.type, request->msg.i.create.mode, &id);

	SEMAPHORE_TRACE("create: id = %d, ret = %d", id, ret);

	rq_setResponse(request, ret);
	request->msg.o.create.oid.port = posixsrv_port();
	request->msg.o.create.oid.id = id;

	return request;
}


static operations_t semaphore_control_ops = {
	.handlers = { NULL },
	.create = semaphore_create_op,
};


int semaphore_init(void)
{
	object_t *object;
	int err;

	object = malloc(sizeof(*object));
	if (object == NULL) {
		return -ENOMEM;
	}

	err = mkdir(SEMAPHORE_PATH, 0777);
	if (err != 0) {
		return -errno;
	}

	posixsrv_object_create(object, &semaphore_control_ops);
	err = posixsrv_object_link(object, SEMCTL_PATH);
	posixsrv_object_put(object);
	return err;
}
