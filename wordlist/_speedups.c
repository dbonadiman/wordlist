/*
 * wordlist._speedups
 * ------------------
 * Optional C accelerator for the wordlist generator.
 *
 * It builds the output directly into a reusable byte buffer with plain
 * memcpy and streams it to a file descriptor, avoiding the per-word
 * object churn and str.join dispatch overhead of the pure-Python path.
 *
 * The module is purely optional: wordlist falls back to the pure-Python
 * implementation when it is not built or when a charset/pattern is not
 * pure ASCII (so the fast path can never change the bytes that are
 * produced).
 */
#define _FILE_OFFSET_BITS 64   /* 64-bit off_t for pwrite on 32-bit builds */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#ifdef MS_WINDOWS
#  include <io.h>
#  define wl_write _write
#else
#  include <unistd.h>
#  define wl_write write
#endif

/* Default output buffer size: amortises write() syscalls. */
#define WL_OUTBUF (1 << 20)

/* Slack so a row copy may overshoot to the next 8-byte boundary safely. */
#define WL_SLACK 8

/*
 * Copy `width` bytes from src to dst, rounding the work up to whole
 * 8-byte stores.  src must have >= WL_SLACK padding and dst must have
 * >= WL_SLACK slack past the logical end of the buffer.  The few extra
 * bytes are either overwritten by the following row or left unused in
 * the buffer tail (never flushed), so this stays correct while letting
 * the compiler emit single-instruction stores for the common short row.
 */
static inline void
wl_copy(char *dst, const char *src, size_t width)
{
    size_t i = 0;
    do {
        memcpy(dst + i, src + i, 8);
        i += 8;
    } while (i < width);
}

/* Write the first `len` bytes of `buf` to `fd`, retrying short writes. */
static int
wl_flush(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        Py_ssize_t n;
        Py_BEGIN_ALLOW_THREADS
        n = (Py_ssize_t) wl_write(fd, buf + off, (unsigned) (len - off));
        Py_END_ALLOW_THREADS
        if (n < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return -1;
        }
        off += (size_t) n;
    }
    return 0;
}

/*
 * Emit the full cartesian product described by a pre-filled row template.
 *
 *   row      : template of `width` bytes, with fixed positions already set
 *              and each variable position initialised to charset[0].
 *   varoff   : byte offsets inside `row` of the `nvar` variable positions,
 *              left to right (left-most is the most significant digit).
 *   charset  : `k` single-byte characters.
 *
 * Rows are produced in the same lexicographic order as
 * itertools.product (right-most variable position changes fastest) and
 * appended to `out`, which is flushed to `fd` as it fills.
 */
static int
wl_emit(int fd, char *out, size_t bufsize, size_t *outlen_io,
        char *row, size_t width,
        const Py_ssize_t *varoff, Py_ssize_t nvar,
        const char *charset, Py_ssize_t k)
{
    Py_ssize_t *idx;
    size_t outlen = *outlen_io;
    int rc = 0;

    if (nvar > 0 && k <= 0) {
        /* a variable position with an empty charset yields nothing */
        return 0;
    }

    if (nvar == 0) {
        /* no variable positions: a single row */
        if (outlen + width > bufsize) {
            if (outlen && wl_flush(fd, out, outlen) < 0) {
                return -1;
            }
            outlen = 0;
        }
        wl_copy(out + outlen, row, width);
        *outlen_io = outlen + width;
        return 0;
    }

    idx = (Py_ssize_t *) PyMem_Calloc(nvar, sizeof(Py_ssize_t));
    if (idx == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    {
        /*
         * Emit the inner-most variable position as a tight burst of `k`
         * rows (it just cycles through the charset with no carry), then
         * advance the remaining positions once per burst.  This keeps the
         * odometer and the buffer-space check off the hot per-row path.
         */
        Py_ssize_t inner = varoff[nvar - 1];
        size_t burst = (size_t) k * width;

        for (;;) {
            char *dst;
            Py_ssize_t j, pos;

            if (outlen + burst > bufsize) {
                if (outlen && wl_flush(fd, out, outlen) < 0) {
                    rc = -1;
                    goto done;
                }
                outlen = 0;
            }
            dst = out + outlen;
            for (j = 0; j < k; j++) {
                row[inner] = charset[j];
                wl_copy(dst, row, width);
                dst += width;
            }
            outlen += burst;

            /* carry into the positions above the inner one */
            row[inner] = charset[0];
            pos = nvar - 2;
            while (pos >= 0) {
                if (++idx[pos] < k) {
                    row[varoff[pos]] = charset[idx[pos]];
                    break;
                }
                idx[pos] = 0;
                row[varoff[pos]] = charset[0];
                pos--;
            }
            if (pos < 0) {
                break;  /* every higher digit wrapped: product exhausted */
            }
        }
    }

    *outlen_io = outlen;
done:
    PyMem_Free(idx);
    return rc;
}

/* Pick an output buffer that holds the default size and at least one burst. */
static size_t
wl_bufsize(size_t burst)
{
    size_t need = burst + 64;
    return need > WL_OUTBUF ? need : WL_OUTBUF;
}

/* Allocate a row template of `width` bytes plus copy slack, zero-padded. */
static char *
wl_alloc_row(size_t width)
{
    char *row = (char *) PyMem_Malloc(width + WL_SLACK);
    if (row != NULL) {
        memset(row + width, 0, WL_SLACK);
    }
    return row;
}

/* ----------------------------------------------------------------------
 * Parallel path (POSIX threads).  Partitions the product by its first
 * variable position into k slabs and writes them concurrently with
 * pwrite() at their known file offsets.  Only usable on a seekable
 * (regular) file; the Python layer falls back to the serial path
 * otherwise.  Output is byte-for-byte identical to the serial path.
 * ------------------------------------------------------------------- */
#ifndef MS_WINDOWS
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>

#define WL_HAS_PARALLEL 1

/* Write all `len` bytes at file offset `offset`; returns 0 or an errno. */
static int
wl_pwrite_all(int fd, const char *buf, size_t len, off_t offset)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = pwrite(fd, buf + off, len - off, offset + (off_t) off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno ? errno : EIO;
        }
        off += (size_t) n;
    }
    return 0;
}

/*
 * Like wl_emit but writes via pwrite at an absolute offset and makes no
 * Python C-API calls (the caller has released the GIL).  Iterates the
 * variable positions varoff[0..nvar-1]; any other position in `row` is
 * already fixed.  Returns 0 on success or an errno.
 */
static int
wl_emit_pwrite(int fd, off_t offset, char *buf, size_t bufsize,
               char *row, size_t width,
               const Py_ssize_t *varoff, Py_ssize_t nvar,
               const char *charset, Py_ssize_t k)
{
    size_t outlen = 0;
    Py_ssize_t *idx;
    int err = 0;

    if (nvar == 0) {
        wl_copy(buf, row, width);
        return wl_pwrite_all(fd, buf, width, offset);
    }
    if (k <= 0) {
        return 0;
    }

    idx = (Py_ssize_t *) calloc((size_t) nvar, sizeof(Py_ssize_t));
    if (idx == NULL) {
        return ENOMEM;
    }

    {
        Py_ssize_t inner = varoff[nvar - 1];
        size_t burst = (size_t) k * width;

        for (;;) {
            char *dst;
            Py_ssize_t j, pos;

            if (outlen + burst > bufsize) {
                if (outlen) {
                    err = wl_pwrite_all(fd, buf, outlen, offset);
                    if (err) {
                        goto done;
                    }
                    offset += (off_t) outlen;
                    outlen = 0;
                }
            }
            dst = buf + outlen;
            for (j = 0; j < k; j++) {
                row[inner] = charset[j];
                wl_copy(dst, row, width);
                dst += width;
            }
            outlen += burst;

            row[inner] = charset[0];
            pos = nvar - 2;
            while (pos >= 0) {
                if (++idx[pos] < k) {
                    row[varoff[pos]] = charset[idx[pos]];
                    break;
                }
                idx[pos] = 0;
                row[varoff[pos]] = charset[0];
                pos--;
            }
            if (pos < 0) {
                break;
            }
        }
        if (outlen) {
            err = wl_pwrite_all(fd, buf, outlen, offset);
            if (err) {
                goto done;
            }
        }
    }
done:
    free(idx);
    return err;
}

/* One product region to generate: its template, variable positions, and
 * placement in the output file. */
typedef struct {
    size_t width;
    Py_ssize_t nvar;       /* number of variable positions, incl. the first */
    Py_ssize_t *varoff;    /* nvar offsets, ascending; [0] is the partition */
    char *templ;           /* width + WL_SLACK, fixed bytes set, vars=charset[0] */
    off_t base;            /* file offset where this region starts */
    size_t subslab;        /* bytes produced per first-variable value */
} wl_spec;

typedef struct {
    int fd;
    const char *charset;
    Py_ssize_t k;
    wl_spec *specs;
    Py_ssize_t nspecs;
    size_t maxwidth;
    size_t maxburst;
    pthread_mutex_t lock;
    Py_ssize_t next;       /* next work-unit index */
    Py_ssize_t nunits;     /* nspecs * k */
    int err;               /* first errno seen, 0 on success */
} wl_pjob;

/* Worker: pull (spec, first-char) units and pwrite each slab. */
static void *
wl_worker(void *arg)
{
    wl_pjob *job = (wl_pjob *) arg;
    size_t bufcap = wl_bufsize(job->maxburst);
    char *buf = (char *) malloc(bufcap + WL_SLACK);
    char *row = (char *) malloc(job->maxwidth + WL_SLACK);

    if (buf == NULL || row == NULL) {
        pthread_mutex_lock(&job->lock);
        if (!job->err) {
            job->err = ENOMEM;
        }
        pthread_mutex_unlock(&job->lock);
        free(buf);
        free(row);
        return NULL;
    }

    for (;;) {
        Py_ssize_t u, si, c;
        wl_spec *sp;
        off_t off;
        int err;

        pthread_mutex_lock(&job->lock);
        if (job->err || job->next >= job->nunits) {
            pthread_mutex_unlock(&job->lock);
            break;
        }
        u = job->next++;
        pthread_mutex_unlock(&job->lock);

        si = u / job->k;
        c = u % job->k;
        sp = &job->specs[si];

        memcpy(row, sp->templ, sp->width + WL_SLACK);
        row[sp->varoff[0]] = job->charset[c];
        off = sp->base + (off_t) c * (off_t) sp->subslab;

        err = wl_emit_pwrite(job->fd, off, buf, bufcap, row, sp->width,
                             sp->varoff + 1, sp->nvar - 1,
                             job->charset, job->k);
        if (err) {
            pthread_mutex_lock(&job->lock);
            if (!job->err) {
                job->err = err;
            }
            pthread_mutex_unlock(&job->lock);
            break;
        }
    }

    free(buf);
    free(row);
    return NULL;
}

/* Run `job` across up to `nthreads` threads (the caller thread helps). */
static void
wl_run_parallel(wl_pjob *job, int nthreads)
{
    pthread_t *tids;
    int i, made = 0;

    if (nthreads < 1) {
        nthreads = 1;
    }
    tids = (pthread_t *) malloc(sizeof(pthread_t) * (size_t) nthreads);
    if (tids == NULL) {
        nthreads = 1;
    }

    Py_BEGIN_ALLOW_THREADS
    for (i = 0; i < nthreads - 1; i++) {
        if (pthread_create(&tids[i], NULL, wl_worker, job) == 0) {
            made++;
        }
        else {
            break;
        }
    }
    wl_worker(job);
    for (i = 0; i < made; i++) {
        pthread_join(tids[i], NULL);
    }
    Py_END_ALLOW_THREADS

    free(tids);
}

/* Multiply two sizes, flagging overflow. */
static int
wl_mul_ovf(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > (size_t) -1 / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static void
wl_free_specs(wl_spec *specs, Py_ssize_t n)
{
    Py_ssize_t i;
    if (specs == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(specs[i].varoff);
        free(specs[i].templ);
    }
    free(specs);
}

/* Raise an OSError from a stored errno (used after the threaded region). */
static PyObject *
wl_oserror(int err)
{
    errno = err;
    PyErr_SetFromErrno(PyExc_OSError);
    return NULL;
}

#else
#define WL_HAS_PARALLEL 0
#endif /* not MS_WINDOWS */


/*
 * write_words(fd, charset_bytes, delim_bytes, minlen, maxlen)
 *
 * Generate every word of length minlen..maxlen over `charset`, each
 * followed by `delim`, writing the result straight to `fd`.
 */
static PyObject *
py_write_words(PyObject *self, PyObject *args)
{
    int fd;
    const char *charset, *delim;
    Py_ssize_t k, dlen, minlen, maxlen;
    char *out = NULL;
    size_t bufsize, outlen = 0, maxwidth;
    Py_ssize_t cur;

    if (!PyArg_ParseTuple(args, "iy#y#nn", &fd, &charset, &k,
                          &delim, &dlen, &minlen, &maxlen)) {
        return NULL;
    }
    if (minlen < 1 || maxlen < minlen) {
        PyErr_SetString(PyExc_ValueError, "invalid length range");
        return NULL;
    }
    if (k <= 0) {
        Py_RETURN_NONE;  /* empty charset: nothing to generate */
    }

    maxwidth = (size_t) maxlen + (size_t) dlen;
    bufsize = wl_bufsize((size_t) k * maxwidth);
    out = (char *) PyMem_Malloc(bufsize + WL_SLACK);
    if (out == NULL) {
        return PyErr_NoMemory();
    }

    for (cur = minlen; cur <= maxlen; cur++) {
        size_t width = (size_t) cur + (size_t) dlen;
        char *row;
        Py_ssize_t *varoff;
        Py_ssize_t i;

        row = wl_alloc_row(width);
        varoff = (Py_ssize_t *) PyMem_Malloc((size_t) cur * sizeof(Py_ssize_t));
        if (row == NULL || varoff == NULL) {
            PyMem_Free(row);
            PyMem_Free(varoff);
            PyMem_Free(out);
            return PyErr_NoMemory();
        }
        for (i = 0; i < cur; i++) {
            row[i] = charset[0];
            varoff[i] = i;
        }
        if (dlen) {
            memcpy(row + cur, delim, (size_t) dlen);
        }

        if (wl_emit(fd, out, bufsize, &outlen, row, width,
                    varoff, cur, charset, k) < 0) {
            PyMem_Free(row);
            PyMem_Free(varoff);
            PyMem_Free(out);
            return NULL;
        }
        PyMem_Free(row);
        PyMem_Free(varoff);
    }

    if (outlen && wl_flush(fd, out, outlen) < 0) {
        PyMem_Free(out);
        return NULL;
    }
    PyMem_Free(out);
    Py_RETURN_NONE;
}

/*
 * write_pattern(fd, charset_bytes, delim_bytes, pattern_bytes)
 *
 * Generate every word matching `pattern` (each '@' is replaced by a
 * charset character, every other byte is fixed), each followed by
 * `delim`, writing the result straight to `fd`.
 */
static PyObject *
py_write_pattern(PyObject *self, PyObject *args)
{
    int fd;
    const char *charset, *delim, *pattern;
    Py_ssize_t k, dlen, plen;
    char *out = NULL, *row = NULL;
    Py_ssize_t *varoff = NULL;
    Py_ssize_t nvar = 0, i;
    size_t width, bufsize, outlen = 0;

    if (!PyArg_ParseTuple(args, "iy#y#y#", &fd, &charset, &k,
                          &delim, &dlen, &pattern, &plen)) {
        return NULL;
    }

    width = (size_t) plen + (size_t) dlen;
    row = wl_alloc_row(width);
    varoff = (Py_ssize_t *) PyMem_Malloc((size_t) (plen > 0 ? plen : 1)
                                         * sizeof(Py_ssize_t));
    if (row == NULL || varoff == NULL) {
        PyMem_Free(row);
        PyMem_Free(varoff);
        return PyErr_NoMemory();
    }

    for (i = 0; i < plen; i++) {
        if (pattern[i] == '@') {
            varoff[nvar++] = i;
            row[i] = (k > 0) ? charset[0] : 0;
        }
        else {
            row[i] = pattern[i];
        }
    }
    if (dlen) {
        memcpy(row + plen, delim, (size_t) dlen);
    }

    if (nvar == 0 || (k <= 0 && nvar > 0)) {
        /* no placeholders, or placeholders with an empty charset */
        PyMem_Free(row);
        PyMem_Free(varoff);
        Py_RETURN_NONE;
    }

    bufsize = wl_bufsize((size_t) k * width);
    out = (char *) PyMem_Malloc(bufsize + WL_SLACK);
    if (out == NULL) {
        PyMem_Free(row);
        PyMem_Free(varoff);
        return PyErr_NoMemory();
    }

    if (wl_emit(fd, out, bufsize, &outlen, row, width,
                varoff, nvar, charset, k) < 0) {
        goto error;
    }
    if (outlen && wl_flush(fd, out, outlen) < 0) {
        goto error;
    }

    PyMem_Free(out);
    PyMem_Free(row);
    PyMem_Free(varoff);
    Py_RETURN_NONE;

error:
    PyMem_Free(out);
    PyMem_Free(row);
    PyMem_Free(varoff);
    return NULL;
}

#if WL_HAS_PARALLEL
/*
 * write_words_parallel(fd, charset, delim, minlen, maxlen, nthreads)
 *
 * Same output as write_words, generated concurrently and written with
 * pwrite at computed offsets.  `fd` must be a seekable regular file.
 * Raises OverflowError if the output size would not fit the offset math
 * (the caller then falls back to the serial path).
 */
static PyObject *
py_write_words_parallel(PyObject *self, PyObject *args)
{
    int fd, nthreads;
    const char *charset, *delim;
    Py_ssize_t k, dlen, minlen, maxlen, cur, nspecs, si;
    wl_spec *specs;
    size_t base = 0, maxwidth = 0, maxburst = 0;
    wl_pjob job;

    if (!PyArg_ParseTuple(args, "iy#y#nni", &fd, &charset, &k,
                          &delim, &dlen, &minlen, &maxlen, &nthreads)) {
        return NULL;
    }
    if (minlen < 1 || maxlen < minlen) {
        PyErr_SetString(PyExc_ValueError, "invalid length range");
        return NULL;
    }
    if (k <= 0) {
        Py_RETURN_NONE;
    }

    nspecs = maxlen - minlen + 1;
    specs = (wl_spec *) calloc((size_t) nspecs, sizeof(wl_spec));
    if (specs == NULL) {
        return PyErr_NoMemory();
    }

    si = 0;
    for (cur = minlen; cur <= maxlen; cur++, si++) {
        size_t width = (size_t) cur + (size_t) dlen;
        size_t pw = 1, subslab, region, burst;
        Py_ssize_t i;
        char *templ;
        Py_ssize_t *varoff;

        for (i = 0; i < cur - 1; i++) {          /* pw = k^(cur-1) */
            if (wl_mul_ovf(pw, (size_t) k, &pw)) {
                goto overflow;
            }
        }
        if (wl_mul_ovf(pw, width, &subslab) ||
            wl_mul_ovf(subslab, (size_t) k, &region) ||
            base > (size_t) -1 - region) {
            goto overflow;
        }

        templ = (char *) malloc(width + WL_SLACK);
        varoff = (Py_ssize_t *) malloc((size_t) cur * sizeof(Py_ssize_t));
        if (templ == NULL || varoff == NULL) {
            free(templ);
            free(varoff);
            wl_free_specs(specs, si);
            return PyErr_NoMemory();
        }
        for (i = 0; i < cur; i++) {
            templ[i] = charset[0];
            varoff[i] = i;
        }
        if (dlen) {
            memcpy(templ + cur, delim, (size_t) dlen);
        }
        memset(templ + width, 0, WL_SLACK);

        specs[si].width = width;
        specs[si].nvar = cur;
        specs[si].varoff = varoff;
        specs[si].templ = templ;
        specs[si].base = (off_t) base;
        specs[si].subslab = subslab;

        base += region;
        if (width > maxwidth) {
            maxwidth = width;
        }
        burst = (size_t) k * width;
        if (burst > maxburst) {
            maxburst = burst;
        }
    }

    job.fd = fd;
    job.charset = charset;
    job.k = k;
    job.specs = specs;
    job.nspecs = nspecs;
    job.maxwidth = maxwidth;
    job.maxburst = maxburst;
    job.next = 0;
    job.nunits = nspecs * k;
    job.err = 0;
    if (pthread_mutex_init(&job.lock, NULL) != 0) {
        wl_free_specs(specs, nspecs);
        return wl_oserror(errno ? errno : EAGAIN);
    }

    wl_run_parallel(&job, nthreads);
    pthread_mutex_destroy(&job.lock);
    wl_free_specs(specs, nspecs);

    if (job.err) {
        return wl_oserror(job.err);
    }
    Py_RETURN_NONE;

overflow:
    wl_free_specs(specs, si);
    PyErr_SetString(PyExc_OverflowError, "output too large for parallel path");
    return NULL;
}

/*
 * write_pattern_parallel(fd, charset, delim, pattern, nthreads)
 *
 * Concurrent counterpart of write_pattern (see write_words_parallel).
 */
static PyObject *
py_write_pattern_parallel(PyObject *self, PyObject *args)
{
    int fd, nthreads;
    const char *charset, *delim, *pattern;
    Py_ssize_t k, dlen, plen, i, nvar = 0;
    size_t width, pw = 1, subslab, region;
    char *templ;
    Py_ssize_t *varoff;
    wl_spec *specs;
    wl_pjob job;

    if (!PyArg_ParseTuple(args, "iy#y#y#i", &fd, &charset, &k,
                          &delim, &dlen, &pattern, &plen, &nthreads)) {
        return NULL;
    }
    if (k <= 0) {
        Py_RETURN_NONE;
    }

    width = (size_t) plen + (size_t) dlen;
    templ = (char *) malloc(width + WL_SLACK);
    varoff = (Py_ssize_t *) malloc((size_t) (plen > 0 ? plen : 1)
                                   * sizeof(Py_ssize_t));
    if (templ == NULL || varoff == NULL) {
        free(templ);
        free(varoff);
        return PyErr_NoMemory();
    }
    for (i = 0; i < plen; i++) {
        if (pattern[i] == '@') {
            varoff[nvar++] = i;
            templ[i] = charset[0];
        }
        else {
            templ[i] = pattern[i];
        }
    }
    if (dlen) {
        memcpy(templ + plen, delim, (size_t) dlen);
    }
    memset(templ + width, 0, WL_SLACK);

    if (nvar == 0) {
        free(templ);
        free(varoff);
        Py_RETURN_NONE;
    }

    for (i = 0; i < nvar - 1; i++) {             /* pw = k^(nvar-1) */
        if (wl_mul_ovf(pw, (size_t) k, &pw)) {
            goto overflow;
        }
    }
    if (wl_mul_ovf(pw, width, &subslab) ||
        wl_mul_ovf(subslab, (size_t) k, &region)) {
        goto overflow;
    }
    (void) region;

    specs = (wl_spec *) calloc(1, sizeof(wl_spec));
    if (specs == NULL) {
        free(templ);
        free(varoff);
        return PyErr_NoMemory();
    }
    specs[0].width = width;
    specs[0].nvar = nvar;
    specs[0].varoff = varoff;
    specs[0].templ = templ;
    specs[0].base = 0;
    specs[0].subslab = subslab;

    job.fd = fd;
    job.charset = charset;
    job.k = k;
    job.specs = specs;
    job.nspecs = 1;
    job.maxwidth = width;
    job.maxburst = (size_t) k * width;
    job.next = 0;
    job.nunits = k;
    job.err = 0;
    if (pthread_mutex_init(&job.lock, NULL) != 0) {
        wl_free_specs(specs, 1);
        return wl_oserror(errno ? errno : EAGAIN);
    }

    wl_run_parallel(&job, nthreads);
    pthread_mutex_destroy(&job.lock);
    wl_free_specs(specs, 1);

    if (job.err) {
        return wl_oserror(job.err);
    }
    Py_RETURN_NONE;

overflow:
    free(templ);
    free(varoff);
    PyErr_SetString(PyExc_OverflowError, "output too large for parallel path");
    return NULL;
}
#endif /* WL_HAS_PARALLEL */

static PyMethodDef wl_methods[] = {
    {"write_words", py_write_words, METH_VARARGS,
     "write_words(fd, charset, delim, minlen, maxlen): stream every word "
     "of the given lengths to fd."},
    {"write_pattern", py_write_pattern, METH_VARARGS,
     "write_pattern(fd, charset, delim, pattern): stream every word "
     "matching pattern to fd."},
#if WL_HAS_PARALLEL
    {"write_words_parallel", py_write_words_parallel, METH_VARARGS,
     "write_words_parallel(fd, charset, delim, minlen, maxlen, nthreads): "
     "concurrent write_words to a seekable fd."},
    {"write_pattern_parallel", py_write_pattern_parallel, METH_VARARGS,
     "write_pattern_parallel(fd, charset, delim, pattern, nthreads): "
     "concurrent write_pattern to a seekable fd."},
#endif
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef wl_module = {
    PyModuleDef_HEAD_INIT,
    "wordlist._speedups",
    "C accelerator for wordlist generation.",
    -1,
    wl_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit__speedups(void)
{
    PyObject *module = PyModule_Create(&wl_module);
    if (module == NULL) {
        return NULL;
    }
    /* Lets Python tell whether the parallel entry points exist. */
    PyModule_AddIntConstant(module, "has_parallel", WL_HAS_PARALLEL);
    return module;
}
