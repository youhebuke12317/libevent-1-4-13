/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	detect_monotonic();
	gettime(base, &base->event_tv);
	
	min_heap_ctor(&base->timeheap);
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;
	
	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;
	}
	
	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;

	if (base->event_count_active)
		return (-1);

	if (base->nactivequeues && npriorities != base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);
		}
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (base->event_break)
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

/**************************************************************************
 *
 *				I/O和Timer事件的统一
 * Libevent将Timer和Signal事件都统一到了系统的I/O 的demultiplex机制中了
 *     首先将Timer事件融合到系统I/O多路复用机制中，还是相当清晰的
 * 因为系统的I/O机制像select()和epoll_wait()都允许程序制定一个最大
 * 等待时间（也称为最大超时时间）timeout，即使没有I/O事件发生，它
 * 们也保证能在timeout时间内返回。
 *     那么根据所有Timer事件的最小超时时间来设置系统I/O的timeout时间
 * 当系统I/O返回时，再激活所有就绪的Timer事件就可以了，这样就能将Timer
 * 事件完美的融合到系统的I/O机制中了。
 *     堆是一种经典的数据结构，向堆中插入、删除元素时间复杂度都是O(lgN)，
 * N为堆中元素的个数，而获取最小key值（小根堆）的复杂度为O(1)；因此变成了
 * 管理Timer事件的绝佳人选（当然是非唯一的），libevent就是采用的堆结构。
 *
 *					I/O和Signal事件的统一
 *     Signal是异步事件的经典事例，将Signal事件统一到系统的I/O多路复用中就
 * 不像Timer事件那么自然了，Signal事件的出现对于进程来讲是完全随机的，进程
 * 不能只是测试一个变量来判别是否发生了一个信号，而是必须告诉内核“在此信号
 * 发生时，请执行如下的操作”。
 *     如果当Signal发生时，并不立即调用event的callback函数处理信号，而是设
 * 法通知系统的I/O机制，让其返回，然后再统一和I/O事件以及Timer一起处理，不
 * 就可以了嘛。是的，这也是libevent中使用的方法。
 *     问题的核心在于，当Signal发生时，如何通知系统的I/O多路复用机制，这里
 * 先买个小关子，放到信号处理一节再详细说明，比如使用pipe。
 *
 *****************************************************************************
 *
 *   Libevent的事件主循环主要是通过event_base_loop()函数完成的，
 *   event_base_loop所作的就是持续执行下面的循环。
 *
 *						开始
 *						 |
 *	  如果发现系统时间被向后调整了，就矫正系统时间
 *						 |
 *	  根据timer heap中event的最小超时时间计算系统I/O
 *	      事件多路分发机制的最大等待时间
 *						 | 
 *		更新List wait time ，并清空time cache
 *						 |
 *	   调用系统I/O事件多路复用等待就绪I/O event
 *						 |
 *	  检查signal的激活标记，如果被设置，择激活signal 
 *	       event，并把event插入到激活链表中
 *						 |
 *		  将就绪的I/O事件插入到激活链表中
 *						 |
 *	  检查heap中的time events，将就绪的time event从
 *	         heap上删除，并插入到激活链表中
 *						 | 
 *	  根据优先级处理激活链表中的就绪event，调用其回调
 *				函数执行时间处理
 *						 |
 *					    结束
 *
 ******************************************************************************/
int
event_base_loop(struct event_base *base, int flags)
{
	//事件的回调函数（base->evsel: 回调函数; base->evbase: 回调函数操作对象）
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;

	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	/* 
	 * clear time cache 
	 *
	 * 清空时间缓存
	 * */
	base->tv_cache.tv_sec = 0;

	/*
	 * evsignal_base是全局变量，在处理signal时，用于指名signal所属的event_base实例
	 * */
	if (base->sig.ev_signal_added)
		evsignal_base = base;

	/* 事件主循环 */
	done = 0;
	while (!done) {
		/* 
		 * Terminate the loop if we have been asked to 
		 *
		 * 查看是否需要跳出循环
		 * base->event_gotterm 表示设置为终止循环
		 * base->event_break 表示设置为立即终止循环
		 * */
		// 程序可以调用event_loopexit_cb()设置event_gotterm标记
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		// 调用event_base_loopbreak()设置event_break标记
		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		/*
		 * 校正系统时间，如果系统使用的是非 MONOTONIC 时间，用户可能会向后调整了系统时间
		 * 在timeout_correct函数里，比较last wait time和当前时间，如果当前时间< last wait time
		 * 表明时间有问题，这是需要更新timer_heap中所有定时事件的超时时间。
		 * */
		timeout_correct(base, &tv);

		/*
		 * 根据timer heap中事件的最小超时时间，计算系统I/O demultiplexer的最大等待时间
		 *
		 * base->event_count_active 表示就绪事件的数量
		 * */
		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			// 根据Timer事件计算evsel->dispatch的最大等待时间
			timeout_next(base, &tv_p);
		} else {
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 *
			 * 依然有未处理的就绪时间，就让I/O demultiplexer立即返回，不必等待
			 * 下面会提到，在libevent中，低优先级的就绪事件可能不能立即被处理
			 */
			// 如果还有活动事件，就不要等待，让evsel->dispatch立即返回
			evutil_timerclear(&tv);
		}
		
		/* 
		 * If we have no events, we just exit 
		 *
		 * 如果当前没有注册事件，就退出
		 *
		 * event_haveevents()返回总的注册事件数量
		 * */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* 
		 * update last old time 更新last wait time
		 * */
		gettime(base, &base->event_tv);

		/* 
		 * clear time cache  清空time cache
		 * */
		base->tv_cache.tv_sec = 0;

		/*
		 * 调用系统I/O demultiplexer等待就绪I/O events，可能是epoll_wait，或者select等；
		 * 在evsel->dispatch()中，会把就绪signal event、I/O event插入到激活链表中
		 * */
		res = evsel->dispatch(base, evbase, tv_p);

		if (res == -1)
			return (-1);
		
		/* 将time cache赋值为当前系统时间 */
		gettime(base, &base->tv_cache);

		/* 
		 * 检查heap中的timer events，将就绪的timer event从heap上删除，并插入到激活链表中 
		 *
		 * 处理超时事件，将超时事件插入到激活链表中
		 * */
		timeout_process(base);

		/* 
		 * 调用event_process_active()处理激活链表中的就绪event，调用其回调函数执行事件处理
		 * 该函数会寻找最高优先级（priority值越小优先级越高）的激活事件链表，
		 * 然后处理链表中的所有就绪事件；
		 * 因此低优先级的就绪事件可能得不到及时处理；
		 * */
		if (base->event_count_active) {
			// 处理激活链表中的就绪event，调用其回调函数执行事件处理
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache  循环结束，清空时间缓存 */
	base->tv_cache.tv_sec = 0;

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}

int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

int
event_priority_set(struct event *ev, int pri)
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

/*
 * 参数：ev：指向要注册的事件；
 * tv：超时时间；
 * 函数将ev注册到ev->ev_base上，事件类型由ev->ev_events指明，如果注册成功，ev将被插入到已注册链表中；
 * 如果tv不是NULL，则会同时注册定时事件，将ev添加到timer堆上；
 * 如果其中有一步操作失败，那么函数保证没有事件会被注册，可以讲这相当于一个原子操作。
 * */
int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *base = ev->ev_base;		// 要注册到的event_base
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;		// base使用的系统I/O策略
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 *
	 * 新的timer事件，调用timer heap接口在堆上预留一个位置
	 * 注：这样能保证该操作的原子性：
	 * 向系统I/O机制注册可能会失败，而当在堆上预留成功后，
	 * 定时事件的添加将肯定不会失败；
	 * 而预留位置的可能结果是堆扩充，但是内部元素并不会改变
	 *
	 * ev->ev_flags 标记event信息字段，表明当前状态
	 * base->timeheap 管理定时事件的小根堆 （min_heap结构体）
	 * min_heap_size() 返回 base->timeheap 结构体中的 base->timeheap->n
	 * */
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}

	/* 
	 * 如果事件ev不在已注册事件(I/O事件和信号事件)列表中(EVLIST_INSERTED)或者激活链表中(EVLIST_ACTIVE)，则调用evbase注册事件 
	 * */
	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
		// 调用evbase注册事件
		res = evsel->add(evbase, ev);
		if (res != -1)		// 注册成功，插入event到已注册链表中
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/* 
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 *
	 * 准备添加定时事件
	 */
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/* 
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 *
		 * 如果event已经在定时器堆中，则删除旧的
		 *
		 * EVLIST_TIMEOUT表明event已经在定时器堆中
		 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. 
		 *
		 * 如果事件已经是就绪状态则从激活链表中删除
		 *
		 * ev->ev_res记录了当前激活事件的类型
		 * */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 *
			 * 将ev_callback调用次数设置为0
			 *
			 * ev->ev_ncalls: 事件就绪执行时，调用ev_callback的次数
			 * ev_pncalls：指针，通常指向ev_ncalls或者为NULL
			 *
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			// 将事件从对应的链表中删除
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		/* 计算时间，并插入到timer小根堆中 */
		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		// 将事件加入到对应的链表中
		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}

	return (res);
}

int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* 
	 * An event without a base has not been added 
	 *
	 * ev_base为NULL，表明ev没有被注册
	 * */
	if (ev->ev_base == NULL)
		return (-1);

	/*
	 * 取得ev注册的event_base和eventop指针
	 * */
	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* 
	 * See if we are just active executing this event in a loop 
	 *
	 * 将ev_callback调用次数设置为0
	 * */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	/*
	 * 从对应的链表中删除
	 * */
	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		// EVLIST_INSERTED表明是I/O或者Signal事件，
		// 需要调用I/O demultiplexer注销事件
		return (evsel->del(evbase, ev));
	}

	return (0);
}

void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

/*
 * timeout_next()函数根据堆中具有最小超时值的事件和当前时间来计算等待时间
 * */
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	// 堆的首元素具有最小的超时值
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* 
		 * if no time-based events are active wait for I/O 
		 *
		 * 如果没有定时事件，将等待时间设置为NULL,表示一直阻塞直到有I/O事件发生
		 * */
		*tv_p = NULL;
		return (0);
	}

	// 取得当前时间
	if (gettime(base, &now) == -1)
		return (-1);

	// 如果超时时间<=当前值，不能等待，需要立即返回
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}

	// 计算等待的时间=当前时间-最小的超时时间
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	gettime(base, tv);
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;
}

void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now);

	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);
	}
}

void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;

	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	/* ev可能已经在激活列表中了，避免重复插入 */
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	//记录queue标记
	ev->ev_flags |= queue;
	switch (queue) {
	case EVLIST_INSERTED:		// I/O或Signal事件，加入已注册事件链表
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:			// 就绪事件，加入激活链表
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {		// 定时事件，加入堆
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
