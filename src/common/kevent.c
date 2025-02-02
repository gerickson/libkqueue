/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
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

/* To get asprintf(3) */
#define _GNU_SOURCE

#include <assert.h>
#include <signal.h>

#include "private.h"

static struct kevent null_kev; /* null kevent for when we get passed a NULL eventlist */

static const char *
kevent_filter_dump(const struct kevent *kev)
{
    static __thread char buf[64];

    snprintf(buf, sizeof(buf), "%d (%s)",
            kev->filter, filter_name(kev->filter));
    return ((const char *) buf);
}

static const char *
kevent_fflags_dump(const struct kevent *kev)
{
    static __thread char buf[1024];
    size_t len;

#define KEVFFL_DUMP(attrib) \
    if (kev->fflags & attrib) \
    strncat((char *) buf, #attrib" ", 64);

    snprintf(buf, sizeof(buf), "fflags=0x%04x (", kev->fflags);
    switch (kev->filter) {
    case EVFILT_VNODE:
        KEVFFL_DUMP(NOTE_DELETE);
        KEVFFL_DUMP(NOTE_WRITE);
        KEVFFL_DUMP(NOTE_EXTEND);
        KEVFFL_DUMP(NOTE_ATTRIB);
        KEVFFL_DUMP(NOTE_LINK);
        KEVFFL_DUMP(NOTE_RENAME);
        break;

    case EVFILT_USER:
        KEVFFL_DUMP(NOTE_FFNOP);
        KEVFFL_DUMP(NOTE_FFAND);
        KEVFFL_DUMP(NOTE_FFOR);
        KEVFFL_DUMP(NOTE_FFCOPY);
        KEVFFL_DUMP(NOTE_TRIGGER);
        break;

    case EVFILT_READ:
    case EVFILT_WRITE:
#ifdef NOTE_LOWAT
        KEVFFL_DUMP(NOTE_LOWAT);
#endif
        break;

    case EVFILT_PROC:
        KEVFFL_DUMP(NOTE_EXIT);
        KEVFFL_DUMP(NOTE_FORK);
        KEVFFL_DUMP(NOTE_EXEC);
        break;

    default:
        break;
    }
    len = strlen(buf);
    if (buf[len - 1] == ' ') buf[len - 1] = '\0';    /* Trim trailing space */
    strcat(buf, ")");

#undef KEVFFL_DUMP

    return ((const char *) buf);
}

static const char *
kevent_flags_dump(const struct kevent *kev)
{
    static __thread char buf[1024];
    size_t len;

#define KEVFL_DUMP(attrib) \
    if (kev->flags & attrib) \
    strncat((char *) buf, #attrib" ", 64);

    snprintf(buf, sizeof(buf), "flags=0x%04x (", kev->flags);
    KEVFL_DUMP(EV_ADD);
    KEVFL_DUMP(EV_ENABLE);
    KEVFL_DUMP(EV_DISABLE);
    KEVFL_DUMP(EV_DELETE);
    KEVFL_DUMP(EV_ONESHOT);
    KEVFL_DUMP(EV_CLEAR);
    KEVFL_DUMP(EV_EOF);
    KEVFL_DUMP(EV_ERROR);
    KEVFL_DUMP(EV_DISPATCH);
    KEVFL_DUMP(EV_RECEIPT);

    len = strlen(buf);
    if (buf[len - 1] == ' ') buf[len - 1] = '\0';    /* Trim trailing space */
    strcat(buf, ")");

#undef KEVFL_DUMP

    return ((const char *) buf);
}

const char *
kevent_dump(const struct kevent *kev)
{
    static __thread char buf[2147];

    snprintf((char *) buf, sizeof(buf),
            "{ ident=%i, filter=%s, %s, %s, data=%d, udata=%p }",
            (u_int) kev->ident,
            kevent_filter_dump(kev),
            kevent_flags_dump(kev),
            kevent_fflags_dump(kev),
            (int) kev->data,
            kev->udata);

    return ((const char *) buf);
}

static int
kevent_copyin_one(struct kqueue *kq, const struct kevent *src)
{
    struct knote  *kn = NULL;
    struct filter *filt;
    int rv = 0;

    if (src->flags & EV_DISPATCH && src->flags & EV_ONESHOT) {
        dbg_puts("Error: EV_DISPATCH and EV_ONESHOT are mutually exclusive");
        errno = EINVAL;
        return (-1);
    }

    if (filter_lookup(&filt, kq, src->filter) < 0)
        return (-1);

    dbg_printf("src=%s", kevent_dump(src));

    kn = knote_lookup(filt, src->ident);
    if (kn == NULL) {
        if (src->flags & EV_ADD) {
            if ((kn = knote_new()) == NULL) {
                errno = ENOENT;
                return (-1);
            }
            memcpy(&kn->kev, src, sizeof(kn->kev));
            kn->kev.flags &= ~EV_ENABLE;
            kn->kev.flags |= EV_ADD;//FIXME why?
            kn->kn_kq = kq;
            assert(filt->kn_create);
            if (filt->kn_create(filt, kn) < 0) {
                dbg_puts("kn_create failed");

                kn->kn_flags |= KNFL_KNOTE_DELETED;
                knote_release(kn);

                errno = EFAULT;
                return (-1);
            }
            knote_insert(filt, kn);
            dbg_printf("kn=%p - created knote %s", kn, kevent_dump(src));

/* XXX- FIXME Needs to be handled in kn_create() to prevent races */
            if (src->flags & EV_DISABLE) {
                kn->kev.flags |= EV_DISABLE;
                return (filt->kn_disable(filt, kn));
            }
            //........................................

            return (0);
        } else {
            dbg_printf("ident=%u - no knote found", (unsigned int)src->ident);
            errno = ENOENT;
            return (-1);
        }
    } else {
        dbg_printf("kn=%p - resolved ident=%i to knote", kn, (int)src->ident);
    }

    if (src->flags & EV_DELETE) {
        rv = knote_delete(filt, kn);
    } else if (src->flags & EV_DISABLE) {
        rv = knote_disable(filt, kn);
    } else if (src->flags & EV_ENABLE) {
        rv = knote_enable(filt, kn);
    } else if (src->flags & EV_ADD || src->flags == 0 || src->flags & EV_RECEIPT) {
        rv = filt->kn_modify(filt, kn, src);

        /*
         * Implement changes common to all filters
         */
        if (rv == 0) {
            /* update udata */
            kn->kev.udata = src->udata;

            /* sync up the dispatch bit */
            COPY_FLAGS_BIT(kn->kev, (*src), EV_DISPATCH);
        }
        dbg_printf("kn=%p - kn_modify rv=%d", kn, rv);
    }

    return (rv);
}

/** @return number of events added to the eventlist */
static int
kevent_copyin(struct kqueue *kq, const struct kevent *changelist, int nchanges,
        struct kevent *eventlist, int nevents)
{
    int status;
    struct kevent *el_p = eventlist, *el_end = el_p + nevents;

    dbg_printf("nchanges=%d nevents=%d", nchanges, nevents);

    for (struct kevent const *cl_p = changelist, *cl_end = cl_p + nchanges;
         cl_p < cl_end;
         cl_p++) {
        if (kevent_copyin_one(kq, cl_p) < 0) {
            dbg_printf("errno=%s", strerror(errno));
            status = errno;
        } else if (cl_p->flags & EV_RECEIPT) {
            status = 0;
        } else {
            continue;
        }

        /*
         * We're out of kevent entries, just return -1 and leave
         * the errno set.  This is... odd, because it means the
         * caller won't have any idea which entries in the
         * changelist were processed.  But I guess the
         * requirement here is for the caller to always provide
         * a kevent array with >= entries as the changelist.
         */
        if (el_p == el_end)
            return (-1);

        memcpy(el_p, cl_p, sizeof(*cl_p));
        el_p->flags |= EV_ERROR; /* This is set both on error and for EV_RECEIPT */
        el_p->data = status;
        el_p++;

        errno = 0; /* Reset back to 0 if we recorded the error as a kevent */
    }

    return (el_p - eventlist);
}

int VISIBLE
kevent(int kqfd, const struct kevent *changelist, int nchanges,
        struct kevent *eventlist, int nevents,
        const struct timespec *timeout)
{
    struct kqueue *kq;
    struct kevent *el_p, *el_end;
    int rv = 0;
#ifndef NDEBUG
    static atomic_uint _kevent_counter = 0;
    unsigned int myid = 0;

    (void) myid;
#endif

    /* deal with ubsan "runtime error: applying zero offset to null pointer" */
    if (eventlist) {
        if (nevents > MAX_KEVENT)
            nevents = MAX_KEVENT;

        el_p = eventlist;
        el_end = el_p + nevents;
    } else {
        eventlist = el_p = el_end = &null_kev;
    }
    if (!changelist) changelist = &null_kev;

    /* Convert the descriptor into an object pointer */
    kq = kqueue_lookup(kqfd);
    if (kq == NULL) {
        errno = ENOENT;
        return (-1);
    }

#ifndef NDEBUG
    if (DEBUG_KQUEUE) {
        myid = atomic_inc(&_kevent_counter);
        dbg_printf("--- START kevent %u --- (nchanges = %d nevents = %d)", myid, nchanges, nevents);
    }
#endif

    /*
     * Process each kevent on the changelist.
     */
    if (nchanges > 0) {
        kqueue_lock(kq);
        rv = kevent_copyin(kq, changelist, nchanges, el_p, el_end - el_p);
        kqueue_unlock(kq);
        dbg_printf("(%u) kevent_copyin rv=%d", myid, rv);
        if (rv < 0)
            goto out;
        if (rv > 0) {
            el_p += rv; /* EV_RECEIPT and EV_ERROR entries */
        }
    }

    /*
     * If we have space remaining after processing
     * the changelist, copy events out.
     */
    if ((el_end - el_p) > 0) {
        rv = kqops.kevent_wait(kq, nevents, timeout);
        dbg_printf("kqops.kevent_wait rv=%i", rv);
        if (likely(rv > 0)) {
            kqueue_lock(kq);
            rv = kqops.kevent_copyout(kq, rv, el_p, el_end - el_p);
            kqueue_unlock(kq);
            dbg_printf("(%u) kevent_copyout rv=%i", myid, rv);
            if (rv >= 0) {
                el_p += rv;             /* Add events from copyin */
                rv = el_p - eventlist;  /* recalculate rv to be the total events in the eventlist */
            }
        } else if (rv == 0) {
            /* Timeout reached */
            dbg_printf("(%u) kevent_wait timedout", myid);
        } else {
            dbg_printf("(%u) kevent_wait failed", myid);
            goto out;
        }
    /*
     * If we have no space, return how many kevents
     * were added by copyin.
     */
    } else {
        rv = el_p - eventlist;
    }

#ifndef NDEBUG
    if (DEBUG_KQUEUE && (rv > 0)) {
        int n;

        dbg_printf("(%u) returning %zu events", myid, el_p - eventlist);
        for (n = 0; n < el_p - eventlist; n++) {
            dbg_printf("(%u) eventlist[%d] = %s", myid, n, kevent_dump(&eventlist[n]));
        }
    }
#endif

out:
    dbg_printf("--- END kevent %u ret %d ---", myid, rv);
    return (rv);
}
