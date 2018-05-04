/******************************************************************************

    @file    StateOS: os_msg.c
    @author  Rajmund Szymanski
    @date    04.05.2018
    @brief   This file provides set of functions for StateOS.

 ******************************************************************************

   Copyright (c) 2018 Rajmund Szymanski. All rights reserved.

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.

 ******************************************************************************/

#include "inc/os_msg.h"
#include "inc/os_tsk.h"

/* -------------------------------------------------------------------------- */
void msg_init( msg_t *msg, unsigned limit, void *data )
/* -------------------------------------------------------------------------- */
{
	assert(!port_isr_inside());
	assert(msg);
	assert(limit);
	assert(data);

	port_sys_lock();

	memset(msg, 0, sizeof(msg_t));

	msg->limit = limit;
	msg->data  = data;

	port_sys_unlock();
}

/* -------------------------------------------------------------------------- */
msg_t *msg_create( unsigned limit )
/* -------------------------------------------------------------------------- */
{
	msg_t *msg;

	assert(!port_isr_inside());
	assert(limit);

	port_sys_lock();

	msg = core_sys_alloc(ABOVE(sizeof(msg_t)) + limit);
	msg_init(msg, limit, (void *)((size_t)msg + ABOVE(sizeof(msg_t))));
	msg->res = msg;

	port_sys_unlock();

	return msg;
}

/* -------------------------------------------------------------------------- */
void msg_kill( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	assert(!port_isr_inside());
	assert(msg);

	port_sys_lock();

	msg->count = 0;
	msg->first = 0;
	msg->next  = 0;
	msg->size  = 0;

	core_all_wakeup(msg, E_STOPPED);

	port_sys_unlock();
}

/* -------------------------------------------------------------------------- */
void msg_delete( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	port_sys_lock();

	msg_kill(msg);
	core_sys_free(msg->res);

	port_sys_unlock();
}

/* -------------------------------------------------------------------------- */
static
unsigned priv_msg_count( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	return msg->size;
}

/* -------------------------------------------------------------------------- */
static
unsigned priv_msg_space( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	if (msg->count == 0)
		return msg->limit;
	else
	if (msg->queue == 0 && msg->count + sizeof(unsigned) < msg->limit)
		return msg->limit - msg->count - sizeof(unsigned);
	else
		return 0;
}

/* -------------------------------------------------------------------------- */
static
char priv_msg_getc( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	unsigned i = msg->first;
	char c = msg->data[i++];
	msg->first = (i < msg->limit) ? i : 0;
	msg->count--;
	return c;
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_putc( msg_t *msg, char c )
/* -------------------------------------------------------------------------- */
{
	unsigned i = msg->next;
	msg->data[i++] = c;
	msg->next = (i < msg->limit) ? i : 0;
	msg->count++;
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_get( msg_t *msg, char *data, unsigned size )
/* -------------------------------------------------------------------------- */
{
	while (size--)
		*data++ = priv_msg_getc(msg);
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_put( msg_t *msg, const char *data, unsigned size )
/* -------------------------------------------------------------------------- */
{
	while (size--)
		priv_msg_putc(msg, *data++);
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_getSize( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	if (msg->count == 0)
		msg->size = 0;
	else
		priv_msg_get(msg, (void *)&msg->size, sizeof(unsigned));
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_putSize( msg_t *msg, unsigned size )
/* -------------------------------------------------------------------------- */
{
	if (msg->count == 0)
		msg->size = size;
	else
	if (size > 0)
		priv_msg_put(msg, (const void *)&size, sizeof(unsigned));
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_getUpdate( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	while (msg->queue != 0 && (msg->count == 0 || msg->count + msg->queue->evt.size + sizeof(unsigned) <= msg->limit))
	{
		priv_msg_putSize(msg, msg->queue->evt.size);
		priv_msg_put(msg, msg->queue->tmp.odata, msg->queue->evt.size);
		msg->queue->evt.size = 0;
		core_tsk_wakeup(msg->queue, E_SUCCESS);
	}
}

/* -------------------------------------------------------------------------- */
static
void priv_msg_putUpdate( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	while (msg->queue != 0 && msg->size > msg->queue->evt.size)
		core_tsk_wakeup(msg->queue, E_TIMEOUT);

	if (msg->queue != 0)
	{
		priv_msg_get(msg, msg->queue->tmp.idata, msg->size);
		msg->queue->evt.size -= msg->size;
		priv_msg_getSize(msg);
		core_tsk_wakeup(msg->queue, E_SUCCESS);
	}
}

/* -------------------------------------------------------------------------- */
unsigned msg_take( msg_t *msg, void *data, unsigned size )
/* -------------------------------------------------------------------------- */
{
	unsigned len = 0;

	assert(msg);
	assert(data);

	port_sys_lock();

	if (size > 0)
	{
		if (msg->size > 0)
		{
			if (size >= priv_msg_count(msg))
			{
				priv_msg_get(msg, data, len = msg->size);
				priv_msg_getSize(msg);
				priv_msg_getUpdate(msg);
			}
		}
	}

	port_sys_unlock();

	return len;
}

/* -------------------------------------------------------------------------- */
static
unsigned priv_msg_wait( msg_t *msg, char *data, unsigned size, cnt_t time, unsigned(*wait)(void*,cnt_t) )
/* -------------------------------------------------------------------------- */
{
	unsigned len = 0;

	assert(!port_isr_inside());
	assert(msg);
	assert(data);

	port_sys_lock();

	if (size > 0)
	{
		if (msg->size > 0)
		{
			if (size >= priv_msg_count(msg))
			{
				priv_msg_get(msg, data, len = msg->size);
				priv_msg_getSize(msg);
				priv_msg_getUpdate(msg);
			}
		}
		else
		{
			System.cur->tmp.idata = data;
			System.cur->evt.size = size;
			wait(msg, time);
			len = size - System.cur->evt.size;
		}
	}

	port_sys_unlock();

	return len;
}

/* -------------------------------------------------------------------------- */
unsigned msg_waitUntil( msg_t *msg, void *data, unsigned size, cnt_t time )
/* -------------------------------------------------------------------------- */
{
	return priv_msg_wait(msg, data, size, time, core_tsk_waitUntil);
}

/* -------------------------------------------------------------------------- */
unsigned msg_waitFor( msg_t *msg, void *data, unsigned size, cnt_t delay )
/* -------------------------------------------------------------------------- */
{
	return priv_msg_wait(msg, data, size, delay, core_tsk_waitFor);
}

/* -------------------------------------------------------------------------- */
unsigned msg_give( msg_t *msg, const void *data, unsigned size )
/* -------------------------------------------------------------------------- */
{
	unsigned len = 0;

	assert(msg);
	assert(data);

	port_sys_lock();

	if (size > 0 && size <= msg->limit)
	{
		if (size <= priv_msg_space(msg))
		{
			priv_msg_putSize(msg, len = size);
			priv_msg_put(msg, data, len);
			priv_msg_putUpdate(msg);
		}
	}

	port_sys_unlock();

	return len;
}

/* -------------------------------------------------------------------------- */
static
unsigned priv_msg_send( msg_t *msg, const char *data, unsigned size, cnt_t time, unsigned(*wait)(void*,cnt_t) )
/* -------------------------------------------------------------------------- */
{
	unsigned len = 0;

	assert(!port_isr_inside());
	assert(msg);
	assert(data);

	port_sys_lock();

	if (size > 0 && size <= msg->limit)
	{
		if (size <= priv_msg_space(msg))
		{
			priv_msg_putSize(msg, len = size);
			priv_msg_put(msg, data, len);
			priv_msg_putUpdate(msg);
		}
		else
		{
			System.cur->tmp.odata = data;
			System.cur->evt.size = size;
			wait(msg, time);
			len = size - System.cur->evt.size;
		}
	}

	port_sys_unlock();

	return len;
}

/* -------------------------------------------------------------------------- */
unsigned msg_sendUntil( msg_t *msg, const void *data, unsigned size, cnt_t time )
/* -------------------------------------------------------------------------- */
{
	return priv_msg_send(msg, data, size, time, core_tsk_waitUntil);
}

/* -------------------------------------------------------------------------- */
unsigned msg_sendFor( msg_t *msg, const void *data, unsigned size, cnt_t delay )
/* -------------------------------------------------------------------------- */
{
	return priv_msg_send(msg, data, size, delay, core_tsk_waitFor);
}

/* -------------------------------------------------------------------------- */
unsigned msg_count( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	unsigned cnt;

	assert(msg);

	port_sys_lock();

	cnt = priv_msg_count(msg);

	port_sys_unlock();

	return cnt;
}

/* -------------------------------------------------------------------------- */
unsigned msg_space( msg_t *msg )
/* -------------------------------------------------------------------------- */
{
	unsigned cnt = 0;

	assert(msg);

	port_sys_lock();

	cnt = priv_msg_space(msg);

	port_sys_unlock();

	return cnt;
}

/* -------------------------------------------------------------------------- */
