/*
 * Copyright (c) 2011 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "private.h"

int
posix_kqueue_init(struct kqueue *kq)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, kq->kq_sockfd) < 0) {
        dbg_perror("socketpair");
        kq->kq_sockfd[0] = -1;
        kq->kq_sockfd[1] = -1;
        return (-1);
    }

    return (0);
}

void
posix_kqueue_free(struct kqueue *kq)
{
    if (kq->kq_sockfd[0] != -1)
        (void) close(kq->kq_sockfd[0]);
    if (kq->kq_sockfd[1] != -1)
        (void) close(kq->kq_sockfd[1]);
}