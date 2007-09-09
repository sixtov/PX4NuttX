/****************************************************************************
 * net-sockets.c
 *
 *   Copyright (C) 2007 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name Gregory Nutt nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>
#include <semaphore.h>
#include <assert.h>
#include <sched.h>
#include <errno.h>

#include <net/uip/uip.h>
#include <nuttx/net.h>
#include <nuttx/kmalloc.h>

#include "net-internal.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void _net_semtake(FAR struct socketlist *list)
{
  /* Take the semaphore (perhaps waiting) */

  while (sem_wait(&list->sl_sem) != 0)
    {
      /* The only case that an error should occr here is if
       * the wait was awakened by a signal.
       */

      ASSERT(*get_errno_ptr() == EINTR);
    }
}

#define _net_semgive(list) sem_post(&list->sl_sem)

/****************************************************************************
 * Pulblic Functions
 ****************************************************************************/

/* This is called from the initialization logic to configure the socket layer */

void net_initialize(void)
{
  /* Initialize the uIP layer */

  uip_init();

  /* Initialize the socket lay -- nothing to do */
}

/* Allocate a list of files for a new task */

FAR struct socketlist *net_alloclist(void)
{
  FAR struct socketlist *list;
  list = (FAR struct socketlist*)kzmalloc(sizeof(struct socketlist));
  if (list)
    {
       /* Start with a reference count of one */

       list->sl_crefs = 1;

       /* Initialize the list access mutex */

      (void)sem_init(&list->sl_sem, 0, 1);
    }
  return list;
}

/* Increase the reference count on a file list */

int net_addreflist(FAR struct socketlist *list)
{
  if (list)
    {
       /* Increment the reference count on the list.
        * NOTE: that we disable interrupts to do this
        * (vs. taking the list semaphore).  We do this
        * because file cleanup operations often must be
        * done from the IDLE task which cannot wait
        * on semaphores.
        */

       register irqstate_t flags = irqsave();
       list->sl_crefs++;
       irqrestore(flags);
    }
  return OK;
}

/* Release a reference to the file list */

int net_releaselist(FAR struct socketlist *list)
{
  int crefs;
  if (list)
    {
       /* Decrement the reference count on the list.
        * NOTE: that we disable interrupts to do this
        * (vs. taking the list semaphore).  We do this
        * because file cleanup operations often must be
        * done from the IDLE task which cannot wait
        * on semaphores.
        */

       irqstate_t flags = irqsave();
       crefs = --(list->sl_crefs);
       irqrestore(flags);

       /* If the count decrements to zero, then there is no reference
        * to the structure and it should be deallocated.  Since there
        * are references, it would be an error if any task still held
        * a reference to the list's semaphore.
        */

       if (crefs <= 0)
          {
             /* Destroy the semaphore and release the filelist */

             (void)sem_destroy(&list->sl_sem);
             sched_free(list);
          }
    }
  return OK;
}

int sockfd_allocate(void)
{
  FAR struct socketlist *list;
  int i;

  /* Get the socket list for this task/thread */

  list = sched_getsockets();
  if (list)
    {
      /* Search for a socket structure with no references */

      _net_semtake(list);
      for (i = 0; i < CONFIG_NSOCKET_DESCRIPTORS; i++)
        {
          /* Are there references on this socket? */
          if (!list->sl_sockets[i].s_crefs)
            {
              /* No take the reference and return the index + an offset
               * as the socket descriptor.
               */
               memset(&list->sl_sockets[i], 0, sizeof(struct socket));
               list->sl_sockets[i].s_crefs = 1;
               _net_semgive(list);
               return i + __SOCKFD_OFFSET;
            }
        }
      _net_semgive(list);
    }
  return ERROR;
}

void sockfd_release(int sockfd)
{
  FAR struct socket *psock;

  /* Get the socket structure for this sockfd */

  psock = sockfd_socket(sockfd);
  if (psock)
    {
      /* Take the list semaphore so that there will be no accesses
       * to this socket structure.
       */

      FAR struct socketlist *list = sched_getsockets();
      if (list)
        {
          /* Decrement the count if there the socket will persist
           * after this.
           */

          _net_semtake(list);
          if (psock && psock->s_crefs > 1)
            {
              psock->s_crefs--;
            }
          else
            {
              /* The socket will not persist... reset it */

              memset(psock, 0, sizeof(struct socket));
            }
          _net_semgive(list);
        }
    }
}

FAR struct socket *sockfd_socket(int sockfd)
{
  FAR struct socketlist *list;
  int ndx = sockfd - __SOCKFD_OFFSET;

  if (ndx >=0 && ndx < CONFIG_NSOCKET_DESCRIPTORS)
    {
      list = sched_getsockets();
      if (list)
        {
          return &list->sl_sockets[ndx];
        }
    }
  return NULL;
}
