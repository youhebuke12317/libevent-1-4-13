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
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"

/*
 * 在libevent中，每种I/O demultiplex机制的实现都必须提供这五个函数接口，
 * 来完成自身的初始化、销毁释放；对事件的注册、注销和分发。
 * 比如对于epoll，libevent实现了5个对应的接口函数，
 * 并在初始化时并将eventop的5个函数指针指向这5个函数，
 * 那么程序就可以使用epoll作为I/O demultiplex机制了。
 * */
struct eventop {
	const char *name;
	void *(*init)(struct event_base *);		//初始化
	int (*add)(void *, struct event *);		//注册事件
	int (*del)(void *, st uct event *);		//删除事件
	int (*dispatch)(struct event_base *, void *, struct timeval *);		//事件分发
	void (*dealloc)(struct event_base *, void *);		//注销，释放资源
	/* set if we need to reinitialize the event base */
	int need_reinit;
};

struct event_base {
	/*
	 * evsel和evbase这两个字段的设置可能会让人有些迷惑，
	 * 这里你可以把evsel和evbase看作是类和静态函数的关系，
	 * 比如添加事件时的调用行为：evsel->add(evbase, ev)，实际执行操作的是evbase；
	 * 这相当于class::add(instance, ev)，instance就是class的一个对象实例。
	 *
	 * evsel指向了全局变量static const struct eventop *eventops[]中的一个；
	 * 前面也说过，libevent将系统提供的I/O demultiplex机制统一封装成了eventop结构；
	 * 因此eventops[]包含了select、poll、kequeue和epoll等等其中的若干个全局实例对象。
	 * evbase实际上是一个eventop实例对象；
	 * */
	const struct eventop *evsel;
	void *evbase;

	/* 
	 * active event management 
	 * activequeues是一个二级指针，前面讲过libevent支持事件优先级，因此可以把它看作是数组，
	 * 其中的元素activequeues[priority]是一个链表，链表的每个节点指向一个优先级为priority的就绪事件event
	 * */
	struct event_list **activequeues;
	int nactivequeues;
	
	/* 
	 * eventqueue，链表，保存了所有的注册事件event的指针
	 * */
	struct event_list eventqueue;

	/* 
	 * signal handling info 
	 * sig是由来管理信号的结构体，将在后面信号处理时专门讲解
	 * */
	struct evsignal_info sig;

	// 总事件数
	int event_count;		/* counts number of total events */
	// 就绪事件的数量
	int event_count_active;	/* counts number of active events */

	// 设置为终止循环
	int event_gotterm;		/* Set to terminate loop */
	// 设置为立即终止循环
	int event_break;		/* Set to terminate loop immediately */


	// timeheap是管理定时事件的小根堆，将在后面定时事件处理时专门讲解
	struct min_heap timeheap;

	// vent_tv和tv_cache是libevent用于时间管理的变量
	struct timeval event_tv;
	struct timeval tv_cache;
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
	for((var) = TAILQ_FIRST(head);					\
	    (var) != TAILQ_END(head);					\
	    (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsignal_set_handler(struct event_base *base, int evsignal,
			  void (*fn)(int));
int _evsignal_restore_handler(struct event_base *base, int evsignal);

/* defined in evutil.c */
const char *evutil_getenv(const char *varname);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
