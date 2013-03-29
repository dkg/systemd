/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <utmpx.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/poll.h>

#include "macro.h"
#include "path-util.h"
#include "utmp-wtmp.h"

int utmp_get_runlevel(int *runlevel, int *previous) {
        struct utmpx lookup, *found;
        int r;
        const char *e;

        assert(runlevel);

        /* If these values are set in the environment this takes
         * precedence. Presumably, sysvinit does this to work around a
         * race condition that would otherwise exist where we'd always
         * go to disk and hence might read runlevel data that might be
         * very new and does not apply to the current script being
         * executed. */

        if ((e = getenv("RUNLEVEL")) && e[0] > 0) {
                *runlevel = e[0];

                if (previous) {
                        /* $PREVLEVEL seems to be an Upstart thing */

                        if ((e = getenv("PREVLEVEL")) && e[0] > 0)
                                *previous = e[0];
                        else
                                *previous = 0;
                }

                return 0;
        }

        if (utmpxname(_PATH_UTMPX) < 0)
                return -errno;

        setutxent();

        zero(lookup);
        lookup.ut_type = RUN_LVL;

        if (!(found = getutxid(&lookup)))
                r = -errno;
        else {
                int a, b;

                a = found->ut_pid & 0xFF;
                b = (found->ut_pid >> 8) & 0xFF;

                if (a < 0 || b < 0)
                        r = -EIO;
                else {
                        *runlevel = a;

                        if (previous)
                                *previous = b;
                        r = 0;
                }
        }

        endutxent();

        return r;
}

static void init_timestamp(struct utmpx *store, usec_t t) {
        assert(store);

        zero(*store);

        if (t <= 0)
                t = now(CLOCK_REALTIME);

        store->ut_tv.tv_sec = t / USEC_PER_SEC;
        store->ut_tv.tv_usec = t % USEC_PER_SEC;
}

static void init_entry(struct utmpx *store, usec_t t) {
        struct utsname uts;

        assert(store);

        init_timestamp(store, t);

        zero(uts);

        if (uname(&uts) >= 0)
                strncpy(store->ut_host, uts.release, sizeof(store->ut_host));

        strncpy(store->ut_line, "~", sizeof(store->ut_line));  /* or ~~ ? */
        strncpy(store->ut_id, "~~", sizeof(store->ut_id));
}

static int write_entry_utmp(const struct utmpx *store) {
        int r;

        assert(store);

        /* utmp is similar to wtmp, but there is only one entry for
         * each entry type resp. user; i.e. basically a key/value
         * table. */

        if (utmpxname(_PATH_UTMPX) < 0)
                return -errno;

        setutxent();

        if (!pututxline(store))
                r = -errno;
        else
                r = 0;

        endutxent();

        return r;
}

static int write_entry_wtmp(const struct utmpx *store) {
        assert(store);

        /* wtmp is a simple append-only file where each entry is
        simply appended to * the end; i.e. basically a log. */

        errno = 0;
        updwtmpx(_PATH_WTMPX, store);
        return -errno;
}

static int write_utmp_wtmp(const struct utmpx *store_utmp, const struct utmpx *store_wtmp) {
        int r, s;

        r = write_entry_utmp(store_utmp);
        s = write_entry_wtmp(store_wtmp);

        if (r >= 0)
                r = s;

        /* If utmp/wtmp have been disabled, that's a good thing, hence
         * ignore the errors */
        if (r == -ENOENT)
                r = 0;

        return r;
}

static int write_entry_both(const struct utmpx *store) {
        return write_utmp_wtmp(store, store);
}

int utmp_put_shutdown(void) {
        struct utmpx store;

        init_entry(&store, 0);

        store.ut_type = RUN_LVL;
        strncpy(store.ut_user, "shutdown", sizeof(store.ut_user));

        return write_entry_both(&store);
}

int utmp_put_reboot(usec_t t) {
        struct utmpx store;

        init_entry(&store, t);

        store.ut_type = BOOT_TIME;
        strncpy(store.ut_user, "reboot", sizeof(store.ut_user));

        return write_entry_both(&store);
}

static const char *sanitize_id(const char *id) {
        size_t l;

        assert(id);
        l = strlen(id);

        if (l <= sizeof(((struct utmpx*) NULL)->ut_id))
                return id;

        return id + l - sizeof(((struct utmpx*) NULL)->ut_id);
}

int utmp_put_init_process(const char *id, pid_t pid, pid_t sid, const char *line) {
        struct utmpx store;

        assert(id);

        init_timestamp(&store, 0);

        store.ut_type = INIT_PROCESS;
        store.ut_pid = pid;
        store.ut_session = sid;

        strncpy(store.ut_id, sanitize_id(id), sizeof(store.ut_id));

        if (line)
                strncpy(store.ut_line, path_get_file_name(line), sizeof(store.ut_line));

        return write_entry_both(&store);
}

int utmp_put_dead_process(const char *id, pid_t pid, int code, int status) {
        struct utmpx lookup, store, store_wtmp, *found;

        assert(id);

        setutxent();

        zero(lookup);
        lookup.ut_type = INIT_PROCESS; /* looks for DEAD_PROCESS, LOGIN_PROCESS, USER_PROCESS, too */
        strncpy(lookup.ut_id, sanitize_id(id), sizeof(lookup.ut_id));

        if (!(found = getutxid(&lookup)))
                return 0;

        if (found->ut_pid != pid)
                return 0;

        memcpy(&store, found, sizeof(store));
        store.ut_type = DEAD_PROCESS;
        store.ut_exit.e_termination = code;
        store.ut_exit.e_exit = status;

        zero(store.ut_user);
        zero(store.ut_host);
        zero(store.ut_tv);

        memcpy(&store_wtmp, &store, sizeof(store_wtmp));
        /* wtmp wants the current time */
        init_timestamp(&store_wtmp, 0);

        return write_utmp_wtmp(&store, &store_wtmp);
}


int utmp_put_runlevel(int runlevel, int previous) {
        struct utmpx store;
        int r;

        assert(runlevel > 0);

        if (previous <= 0) {
                /* Find the old runlevel automatically */

                if ((r = utmp_get_runlevel(&previous, NULL)) < 0) {
                        if (r != -ESRCH)
                                return r;

                        previous = 0;
                }
        }

        if (previous == runlevel)
                return 0;

        init_entry(&store, 0);

        store.ut_type = RUN_LVL;
        store.ut_pid = (runlevel & 0xFF) | ((previous & 0xFF) << 8);
        strncpy(store.ut_user, "runlevel", sizeof(store.ut_user));

        return write_entry_both(&store);
}

#define TIMEOUT_MSEC 50

static int write_to_terminal(const char *tty, const char *message) {
        int fd, r;
        const char *p;
        size_t left;
        usec_t end;

        assert(tty);
        assert(message);

        if ((fd = open(tty, O_WRONLY|O_NDELAY|O_NOCTTY|O_CLOEXEC)) < 0)
                return -errno;

        if (!isatty(fd)) {
                r = -errno;
                goto finish;
        }

        p = message;
        left = strlen(message);

        end = now(CLOCK_MONOTONIC) + TIMEOUT_MSEC*USEC_PER_MSEC;

        while (left > 0) {
                ssize_t n;
                struct pollfd pollfd;
                usec_t t;
                int k;

                t = now(CLOCK_MONOTONIC);

                if (t >= end) {
                        r = -ETIME;
                        goto finish;
                }

                zero(pollfd);
                pollfd.fd = fd;
                pollfd.events = POLLOUT;

                if ((k = poll(&pollfd, 1, (end - t) / USEC_PER_MSEC)) < 0)
                        return -errno;

                if (k <= 0) {
                        r = -ETIME;
                        goto finish;
                }

                if ((n = write(fd, p, left)) < 0) {

                        if (errno == EAGAIN)
                                continue;

                        r = -errno;
                        goto finish;
                }

                assert((size_t) n <= left);

                p += n;
                left -= n;
        }

        r = 0;

finish:
        close_nointr_nofail(fd);

        return r;
}

int utmp_wall(const char *message, bool (*match_tty)(const char *tty)) {
        struct utmpx *u;
        char date[FORMAT_TIMESTAMP_MAX];
        char *text = NULL, *hn = NULL, *un = NULL, *tty = NULL;
        int r;

        if (!(hn = gethostname_malloc()) ||
            !(un = getlogname_malloc())) {
                r = -ENOMEM;
                goto finish;
        }

        getttyname_harder(STDIN_FILENO, &tty);

        if (asprintf(&text,
                     "\a\r\n"
                     "Broadcast message from %s@%s%s%s (%s):\r\n\r\n"
                     "%s\r\n\r\n",
                     un, hn,
                     tty ? " on " : "", strempty(tty),
                     format_timestamp(date, sizeof(date), now(CLOCK_REALTIME)),
                     message) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        setutxent();

        r = 0;

        while ((u = getutxent())) {
                int q;
                const char *path;
                char *buf = NULL;

                if (u->ut_type != USER_PROCESS || u->ut_user[0] == 0)
                        continue;

                /* this access is fine, because strlen("/dev/") << 32 (UT_LINESIZE) */
                if (path_startswith(u->ut_line, "/dev/"))
                        path = u->ut_line;
                else {
                        if (asprintf(&buf, "/dev/%.*s",
                                     sizeof(u->ut_line), u->ut_line) < 0) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        path = buf;
                }

                if (!match_tty || match_tty(path))
                        if ((q = write_to_terminal(path, text)) < 0)
                                r = q;

                free(buf);
        }

finish:
        free(hn);
        free(un);
        free(tty);
        free(text);

        return r;
}
