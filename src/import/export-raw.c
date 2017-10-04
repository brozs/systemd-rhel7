/***
  This file is part of systemd.

  Copyright 2015 Lennart Poettering

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

#include <sys/sendfile.h>

/* When we include libgen.h because we need dirname() we immediately
 * undefine basename() since libgen.h defines it as a macro to the POSIX
 * version which is really broken. We prefer GNU basename(). */
#include <libgen.h>
#undef basename

#include "sd-daemon.h"

#include "alloc-util.h"
#include "btrfs-util.h"
#include "copy.h"
#include "export-raw.h"
#include "fd-util.h"
#include "fileio.h"
#include "import-common.h"
#include "missing.h"
#include "ratelimit.h"
#include "string-util.h"
#include "util.h"

#define COPY_BUFFER_SIZE (16*1024)

struct RawExport {
        sd_event *event;

        RawExportFinished on_finished;
        void *userdata;

        char *path;

        int input_fd;
        int output_fd;

        ImportCompress compress;

        sd_event_source *output_event_source;

        void *buffer;
        size_t buffer_size;
        size_t buffer_allocated;

        uint64_t written_compressed;
        uint64_t written_uncompressed;

        unsigned last_percent;
        RateLimit progress_rate_limit;

        struct stat st;

        bool eof;
        bool tried_reflink;
        bool tried_sendfile;
};

RawExport *raw_export_unref(RawExport *e) {
        if (!e)
                return NULL;

        sd_event_source_unref(e->output_event_source);

        import_compress_free(&e->compress);

        sd_event_unref(e->event);

        safe_close(e->input_fd);

        free(e->buffer);
        free(e->path);
        return mfree(e);
}

int raw_export_new(
                RawExport **ret,
                sd_event *event,
                RawExportFinished on_finished,
                void *userdata) {

        _cleanup_(raw_export_unrefp) RawExport *e = NULL;
        int r;

        assert(ret);

        e = new0(RawExport, 1);
        if (!e)
                return -ENOMEM;

        e->output_fd = e->input_fd = -1;
        e->on_finished = on_finished;
        e->userdata = userdata;

        RATELIMIT_INIT(e->progress_rate_limit, 100 * USEC_PER_MSEC, 1);
        e->last_percent = (unsigned) -1;

        if (event)
                e->event = sd_event_ref(event);
        else {
                r = sd_event_default(&e->event);
                if (r < 0)
                        return r;
        }

        *ret = e;
        e = NULL;

        return 0;
}

static void raw_export_report_progress(RawExport *e) {
        unsigned percent;
        assert(e);

        if (e->written_uncompressed >= (uint64_t) e->st.st_size)
                percent = 100;
        else
                percent = (unsigned) ((e->written_uncompressed * UINT64_C(100)) / (uint64_t) e->st.st_size);

        if (percent == e->last_percent)
                return;

        if (!ratelimit_test(&e->progress_rate_limit))
                return;

        sd_notifyf(false, "X_IMPORT_PROGRESS=%u", percent);
        log_info("Exported %u%%.", percent);

        e->last_percent = percent;
}

static int raw_export_process(RawExport *e) {
        ssize_t l;
        int r;

        assert(e);

        if (!e->tried_reflink && e->compress.type == IMPORT_COMPRESS_UNCOMPRESSED) {

                /* If we shall take an uncompressed snapshot we can
                 * reflink source to destination directly. Let's see
                 * if this works. */

                r = btrfs_reflink(e->input_fd, e->output_fd);
                if (r >= 0) {
                        r = 0;
                        goto finish;
                }

                e->tried_reflink = true;
        }

        if (!e->tried_sendfile && e->compress.type == IMPORT_COMPRESS_UNCOMPRESSED) {

                l = sendfile(e->output_fd, e->input_fd, NULL, COPY_BUFFER_SIZE);
                if (l < 0) {
                        if (errno == EAGAIN)
                                return 0;

                        e->tried_sendfile = true;
                } else if (l == 0) {
                        r = 0;
                        goto finish;
                } else {
                        e->written_uncompressed += l;
                        e->written_compressed += l;

                        raw_export_report_progress(e);

                        return 0;
                }
        }

        while (e->buffer_size <= 0) {
                uint8_t input[COPY_BUFFER_SIZE];

                if (e->eof) {
                        r = 0;
                        goto finish;
                }

                l = read(e->input_fd, input, sizeof(input));
                if (l < 0) {
                        r = log_error_errno(errno, "Failed to read raw file: %m");
                        goto finish;
                }

                if (l == 0) {
                        e->eof = true;
                        r = import_compress_finish(&e->compress, &e->buffer, &e->buffer_size, &e->buffer_allocated);
                } else {
                        e->written_uncompressed += l;
                        r = import_compress(&e->compress, input, l, &e->buffer, &e->buffer_size, &e->buffer_allocated);
                }
                if (r < 0) {
                        r = log_error_errno(r, "Failed to encode: %m");
                        goto finish;
                }
        }

        l = write(e->output_fd, e->buffer, e->buffer_size);
        if (l < 0) {
                if (errno == EAGAIN)
                        return 0;

                r = log_error_errno(errno, "Failed to write output file: %m");
                goto finish;
        }

        assert((size_t) l <= e->buffer_size);
        memmove(e->buffer, (uint8_t*) e->buffer + l, e->buffer_size - l);
        e->buffer_size -= l;
        e->written_compressed += l;

        raw_export_report_progress(e);

        return 0;

finish:
        if (r >= 0) {
                (void) copy_times(e->input_fd, e->output_fd);
                (void) copy_xattr(e->input_fd, e->output_fd);
        }

        if (e->on_finished)
                e->on_finished(e, r, e->userdata);
        else
                sd_event_exit(e->event, r);

        return 0;
}

static int raw_export_on_output(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        RawExport *i = userdata;

        return raw_export_process(i);
}

static int raw_export_on_defer(sd_event_source *s, void *userdata) {
        RawExport *i = userdata;

        return raw_export_process(i);
}

static int reflink_snapshot(int fd, const char *path) {
        char *p, *d;
        int new_fd, r;

        p = strdupa(path);
        d = dirname(p);

        new_fd = open(d, O_TMPFILE|O_CLOEXEC|O_NOCTTY|O_RDWR, 0600);
        if (new_fd < 0) {
                _cleanup_free_ char *t = NULL;

                r = tempfn_random(path, NULL, &t);
                if (r < 0)
                        return r;

                new_fd = open(t, O_CLOEXEC|O_CREAT|O_NOCTTY|O_RDWR, 0600);
                if (new_fd < 0)
                        return -errno;

                (void) unlink(t);
        }

        r = btrfs_reflink(fd, new_fd);
        if (r < 0) {
                safe_close(new_fd);
                return r;
        }

        return new_fd;
}

int raw_export_start(RawExport *e, const char *path, int fd, ImportCompressType compress) {
        _cleanup_close_ int sfd = -1, tfd = -1;
        int r;

        assert(e);
        assert(path);
        assert(fd >= 0);
        assert(compress < _IMPORT_COMPRESS_TYPE_MAX);
        assert(compress != IMPORT_COMPRESS_UNKNOWN);

        if (e->output_fd >= 0)
                return -EBUSY;

        r = fd_nonblock(fd, true);
        if (r < 0)
                return r;

        r = free_and_strdup(&e->path, path);
        if (r < 0)
                return r;

        sfd = open(path, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (sfd < 0)
                return -errno;

        if (fstat(sfd, &e->st) < 0)
                return -errno;
        if (!S_ISREG(e->st.st_mode))
                return -ENOTTY;

        /* Try to take a reflink snapshot of the file, if we can t make the export atomic */
        tfd = reflink_snapshot(sfd, path);
        if (tfd >= 0) {
                e->input_fd = tfd;
                tfd = -1;
        } else {
                e->input_fd = sfd;
                sfd = -1;
        }

        r = import_compress_init(&e->compress, compress);
        if (r < 0)
                return r;

        r = sd_event_add_io(e->event, &e->output_event_source, fd, EPOLLOUT, raw_export_on_output, e);
        if (r == -EPERM) {
                r = sd_event_add_defer(e->event, &e->output_event_source, raw_export_on_defer, e);
                if (r < 0)
                        return r;

                r = sd_event_source_set_enabled(e->output_event_source, SD_EVENT_ON);
        }
        if (r < 0)
                return r;

        e->output_fd = fd;
        return r;
}