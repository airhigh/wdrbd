﻿/*
	Copyright(C) 2007-2016, ManTechnology Co., LTD.
	Copyright(C) 2007-2016, wdrbd@mantech.co.kr

	Windows DRBD is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2, or (at your option)
	any later version.

	Windows DRBD is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Windows DRBD; see the file COPYING. If not, write to
	the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <wdm.h>
#include "drbd_windows.h"
#include "loglink.h"
#include "../drbd/drbd-kernel-compat/drbd_wrappers.h"

atomic_t g_loglink_state = LOGLINK_UNINITIALIZED;
int g_loglink_tcp_port;
int g_loglink_usage;
struct loglink_worker loglink = { 0 };
struct mutex loglink_mutex;
NPAGED_LOOKASIDE_LIST loglink_printk_msg;
PETHREAD g_LoglinkServerThread;

static PWSK_SOCKET g_loglink_sock = NULL;
static int send_err_count;

VOID LogLink_MakeUsable()
{
	mutex_init(&loglink_mutex);
	ExInitializeNPagedLookasideList(&loglink_printk_msg, NULL, NULL, 0, MAX_ELOG_BUF, 'AADW', 0);
	INIT_LIST_HEAD(&loglink.loglist);
	atomic_cmpxchg(&g_loglink_state, LOGLINK_UNINITIALIZED, LOGLINK_USABLE);
}

VOID LogLink_MakeUnusable()
{
	ExDeleteNPagedLookasideList(&loglink_printk_msg);
}

BOOLEAN LogLink_IsUsable()
{
	return LOGLINK_USABLE <= atomic_read(&g_loglink_state);
}

BOOLEAN LogLink_IsTransferable()
{
	return LOGLINK_TRANSFERABLE <= atomic_read(&g_loglink_state);
}

NTSTATUS LogLink_QueueBuffer(char* buf)
{
	struct loglink_msg_list  *loglink_msg;
		
	loglink_msg = (struct loglink_msg_list *) ExAllocateFromNPagedLookasideList(&loglink_printk_msg);
	if (loglink_msg == NULL)
	{
		DbgPrint("DRBD_ERROR:loglink: no memory\n");
		return STATUS_NO_MEMORY;
	}
	loglink_msg->buf = buf;
	mutex_lock(&loglink_mutex);
	list_add_tail(&loglink_msg->list, &loglink.loglist);	// Add at tail to send log in chronological order.
	mutex_unlock(&loglink_mutex);

	if (LogLink_IsTransferable())	// If it's not currently transferable, sending data is deferred.
		queue_work(loglink.wq, &loglink.worker);

	return STATUS_SUCCESS;
}

VOID NTAPI LogLink_ListenThread(PVOID p)
{
	PWSK_SOCKET		ListenSock = NULL;
	SOCKADDR_IN		LocalAddress = { 0 }, RemoteAddress = { 0 };
	NTSTATUS		Status = STATUS_UNSUCCESSFUL;
		
	while (1)
	{
		extern LONG	g_SocketsState;
		if (g_SocketsState == INITIALIZED &&
			LOGLINK_USABLE <= atomic_read(&g_loglink_state))
		{
			break;
		}

		LARGE_INTEGER	Interval;
		Interval.QuadPart = (-1 * 100 * 10000);   // 0.1 sec
		KeDelayExecutionThread(KernelMode, FALSE, &Interval);
	}

	DbgPrint("DRBD: LogLink listener start. port=%d\n", g_loglink_tcp_port);

	loglink.wq = create_singlethread_workqueue("loglink");
	if (!loglink.wq) 
	{
		printk(KERN_ERR "LogLink: create_singlethread_workqueue failed\n");
		PsTerminateSystemThread(STATUS_SUCCESS);
	}

	INIT_WORK(&loglink.worker, LogLink_Sender);

	ListenSock = CreateSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSK_FLAG_LISTEN_SOCKET);
	if (ListenSock == NULL) 
	{
		printk(KERN_ERR "LogLink: ListenSock failed\n");
		PsTerminateSystemThread(STATUS_SUCCESS);
	}

	LocalAddress.sin_family = AF_INET;
	LocalAddress.sin_addr.s_addr = INADDR_ANY;
	LocalAddress.sin_port = HTONS(g_loglink_tcp_port);

	LONG InputBuffer = 1;
	Status = ControlSocket(ListenSock, WskSetOption, SO_REUSEADDR, SOL_SOCKET, sizeof(ULONG), &InputBuffer, 0, NULL, NULL);
	if (!NT_SUCCESS(Status)) 
	{
		printk(KERN_ERR "LogLink: SO_REUSEADDR failed = 0x%x\n", Status);
		CloseSocket(ListenSock);
		PsTerminateSystemThread(Status);
	}

	Status = Bind(ListenSock, (PSOCKADDR) &LocalAddress);
	if (!NT_SUCCESS(Status)) {
		printk(KERN_ERR "LogLink: Bind() failed with status 0x%08X\n", Status);
		CloseSocket(ListenSock);
		PsTerminateSystemThread(Status);
		// retry?
	}

	atomic_cmpxchg(&g_loglink_state, LOGLINK_USABLE, LOGLINK_INITIALIZED);

	while (TRUE) 
	{
		PWSK_SOCKET		AcceptSock = NULL;
		static int accept_error_retry = 0;

		if ((AcceptSock = Accept(ListenSock, (PSOCKADDR) &LocalAddress, (PSOCKADDR) &RemoteAddress, &Status, 0)) == NULL)
		{
			if (accept_error_retry++ < 3)
			{
				printk(KERN_ERR "LogLink: accept error=0x%08X. retry(%d)\n", Status, accept_error_retry);
			}

			LARGE_INTEGER	Interval;
			Interval.QuadPart = (-1 * 5000 * 10000);   // 5 sec
			KeDelayExecutionThread(KernelMode, FALSE, &Interval);
			continue;
		}

		// lock? don't care! ignore it.
		if (g_loglink_sock)
		{
			printk(KERN_DEBUG "LogLink: close previous socket first.\n");
			CloseSocket(g_loglink_sock);
			// ignore error
		}

		printk(KERN_INFO "LogLink: accept new loglink socket success. retry(%d)\n", accept_error_retry);
		send_err_count = 0;
		g_loglink_sock = AcceptSock;
		accept_error_retry = 0;
		atomic_cmpxchg(&g_loglink_state, LOGLINK_INITIALIZED, LOGLINK_TRANSFERABLE);
	}

	// not reached here.
	destroy_workqueue(loglink.wq);
	PsTerminateSystemThread(STATUS_SUCCESS);
}

void LogLink_Sender(struct work_struct *ws)
{
	struct loglink_worker *worker = container_of(ws, struct loglink_worker, worker);
	struct loglink_msg_list *msg, *q;
	PWSK_SOCKET	sock = g_loglink_sock;
	int count = 0;

	LIST_HEAD(work_list);	
	mutex_lock(&loglink_mutex);
	list_splice_init(&worker->loglist, &work_list);
	mutex_unlock(&loglink_mutex);

	list_for_each_entry_safe(struct loglink_msg_list, msg, q, &work_list, list)
	{
		int step = 0;
		int ret = 0;

		count++;
		if (sock)
		{
			int sz = strlen(msg->buf);

			if ((ret = SendLocal(sock, &sz, sizeof(int), 0, LOGLINK_TIMEOUT)) != sizeof(int))
			{
				step = 1;
				goto error;
			}

			if ((ret = SendLocal(sock, msg->buf, sz, 0, LOGLINK_TIMEOUT)) != sz)
			{
				step = 2;
				goto error;
			}

			if ((ret = Receive(sock, &sz, sizeof(int), 0, LOGLINK_TIMEOUT)) != sizeof(int))
			{
				step = 3;
				goto error;
			}
		}
		else
		{
			step = 4;

		error:
			if (send_err_count++ == 0)
			{
				// save only one first error after new loglink connection

				char *tmp;
				if ((tmp = kmalloc(512, GFP_KERNEL, 'ACDW')) == NULL)
				{
					DbgPrintEx(FLTR_COMPONENT, DPFLTR_ERROR_LEVEL, "LogLink: malloc fail\n");
				}
				else
				{
					DbgPrintEx(FLTR_COMPONENT, DPFLTR_ERROR_LEVEL, "LogLink: send error: step=%d sock=0x%p ret=%d.\n", step, sock, ret);
					kfree(tmp);
				}
			}
			
			CloseSocket(sock); // just close, no retry/handshake!
			sock = NULL;
		}

		ExFreeToNPagedLookasideList(&drbd_printk_msg, msg->buf);
		list_del(&msg->list);
		ExFreeToNPagedLookasideList(&loglink_printk_msg, msg);
	}
}