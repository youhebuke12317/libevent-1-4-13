/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

/*
 * evsignal_info的初始化包括:
 *			(1) 创建socket pair;
 *			(2) 设置ev_signal事件（但并没有注册，而是等到有信号注册时才检查并注册），并将所有标记置零;
 *			(3) 初始化信号的注册事件链表指针等。
 * */
struct evsignal_info {
	 
	struct event ev_signal;		// // 为socket pair的读socket向event_base注册读事件时使用的event结构体
	int ev_signal_pair[2];		// ev_signal_pair，socket pair对
	int ev_signal_added;		// 记录ev_signal事件是否已经注册

	/* 
	 * volatile提醒编译器它后面所定义的变量随时都有可能改变，
	 * 因此编译后的程序每次需要存储或读取这个变量的时候，都会
	 * 直接从变量地址中读取数据。如果没有volatile关键字，则编
	 * 译器可能优化读取和存储，可能暂时使用寄存器中的值，如果
	 * 这个变量由别的程序更新了的话，将出现不一致的现象。 
	 * */
	volatile sig_atomic_t evsignal_caught;		// 是否有信号发生的标记；是volatile类型，因为它会在另外的线程中被修改
	struct event_list evsigevents[NSIG];		// 数组，evsigevents[signo]表示注册到信号signo的事件链表
	sig_atomic_t evsigcaught[NSIG];				// 具体记录每个信号触发的次数，evsigcaught[signo]是记录信号signo被触发的次数

	/*
	 * sh_old记录了原来的signal处理函数指针，当信号signo注册的event被清空时，需要重新设置其处理函数；
	 * */
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;
#else
	ev_sighandler_t **sh_old;
#endif
	int sh_old_max;
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
