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
 * Output sink: lets the product builder serve both the serial path
 * (sequential writes, GIL held) and the parallel path (pwrite at an
 * advancing offset, GIL released).  Returns 0 on success, -1 on failure.
 */
typedef int (*wl_sink_fn)(void *ctx, const char *buf, size_t len);

typedef struct { int fd; } wl_serial_ctx;

/* Parallel sink: pwrite at an advancing absolute offset (GIL released). */
typedef struct { int fd; off_t offset; int err; } wl_pwrite_ctx;

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

static int
wl_sink_serial(void *ctx, const char *buf, size_t len)
{
    return wl_flush(((wl_serial_ctx *) ctx)->fd, buf, len);
}

/*
 * Build the cartesian product into `out` and stream it through `sink`.
 *
 *   row     : width-byte template; fixed bytes set, every variable
 *             position initialised to charset[0].
 *   varoff  : offsets of the `nvar` variable positions, ascending
 *             (left-most is the most significant digit).
 *
 * Strategy: materialise the right-most `m` variable positions once as a
 * "suffix block" using recursive memcpy doubling -- so most output bytes
 * are replicated at memory-bandwidth speed instead of being generated a
 * row at a time -- where `m` is the largest count whose block fits
 * `bufsize`.  Then walk the remaining "prefix" positions like an
 * odometer, stamping only the changed prefix column into the block and
 * flushing it; the suffix bytes are never recomputed.
 *
 * Rows are produced in the same lexicographic order as itertools.product.
 * Returns 0, -1 (sink failure) or -2 (out of memory).
 */
static int
wl_emit_core(char *out, size_t bufsize, const char *row, size_t width,
             const Py_ssize_t *varoff, Py_ssize_t nvar,
             const char *charset, Py_ssize_t k,
             wl_sink_fn sink, void *ctx)
{
    Py_ssize_t m, nprefix, s, j;
    size_t rows, blk, r;

    if (nvar > 0 && k <= 0) {
        return 0;  /* variable position with empty charset: no output */
    }
    if (nvar == 0) {
        memcpy(out, row, width);
        return sink(ctx, out, width);
    }

    /* Largest m with k^m * width <= bufsize (at least 1, since
     * bufsize >= k * width by construction). */
    m = 0;
    rows = 1;
    while (m < nvar && rows * (size_t) k * width <= bufsize) {
        rows *= (size_t) k;
        m++;
    }

    /* Build the suffix block for varoff[nvar-m .. nvar-1] by doubling. */
    memcpy(out, row, width);
    rows = 1;
    for (s = nvar - 1; s >= nvar - m; s--) {
        Py_ssize_t p = varoff[s];
        size_t bsz = rows * width;
        for (j = 1; j < k; j++) {
            memcpy(out + (size_t) j * bsz, out, bsz);
        }
        for (j = 1; j < k; j++) {
            char ch = charset[j];
            char *col = out + (size_t) j * bsz + p;
            for (r = 0; r < rows; r++) {
                col[r * width] = ch;
            }
        }
        rows *= (size_t) k;
    }
    blk = rows * width;

    nprefix = nvar - m;
    if (nprefix == 0) {
        return sink(ctx, out, blk);
    }

    {
        Py_ssize_t *idx = (Py_ssize_t *) calloc((size_t) nprefix,
                                                sizeof(Py_ssize_t));
        int rc = 0;
        if (idx == NULL) {
            return -2;
        }
        for (;;) {
            Py_ssize_t pos;
            rc = sink(ctx, out, blk);
            if (rc) {
                break;
            }
            /* Odometer over the prefix positions; stamp the changed
             * column across every row of the reused suffix block. */
            pos = nprefix - 1;
            while (pos >= 0) {
                Py_ssize_t p = varoff[pos];
                char ch;
                if (++idx[pos] < k) {
                    ch = charset[idx[pos]];
                    for (r = 0; r < rows; r++) {
                        out[r * width + p] = ch;
                    }
                    break;
                }
                idx[pos] = 0;
                ch = charset[0];
                for (r = 0; r < rows; r++) {
                    out[r * width + p] = ch;
                }
                pos--;
            }
            if (pos < 0) {
                break;
            }
        }
        free(idx);
        return rc;
    }
}

/* Serial product emit: build and write sequentially to `fd`. */
static int
wl_emit(int fd, char *out, size_t bufsize, const char *row, size_t width,
        const Py_ssize_t *varoff, Py_ssize_t nvar,
        const char *charset, Py_ssize_t k)
{
    wl_serial_ctx ctx;
    int rc;
    ctx.fd = fd;
    rc = wl_emit_core(out, bufsize, row, width, varoff, nvar, charset, k,
                      wl_sink_serial, &ctx);
    if (rc == -2) {
        PyErr_NoMemory();
        return -1;
    }
    return rc;  /* 0 ok; -1 means the sink already raised */
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
#include <fcntl.h>
#include <sys/stat.h>

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

static int
wl_sink_pwrite(void *ctx, const char *buf, size_t len)
{
    wl_pwrite_ctx *s = (wl_pwrite_ctx *) ctx;
    int err = wl_pwrite_all(s->fd, buf, len, s->offset);
    if (err) {
        s->err = err;
        return -1;
    }
    s->offset += (off_t) len;
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
    wl_pwrite_ctx ctx;
    int rc;
    ctx.fd = fd;
    ctx.offset = offset;
    ctx.err = 0;
    rc = wl_emit_core(buf, bufsize, row, width, varoff, nvar, charset, k,
                      wl_sink_pwrite, &ctx);
    if (rc == -2) {
        return ENOMEM;
    }
    if (rc < 0) {
        return ctx.err ? ctx.err : EIO;
    }
    return 0;
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
    size_t bufsize, maxwidth;
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

        if (wl_emit(fd, out, bufsize, row, width,
                    varoff, cur, charset, k) < 0) {
            PyMem_Free(row);
            PyMem_Free(varoff);
            PyMem_Free(out);
            return NULL;
        }
        PyMem_Free(row);
        PyMem_Free(varoff);
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
    size_t width, bufsize;

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

    if (wl_emit(fd, out, bufsize, row, width,
                varoff, nvar, charset, k) < 0) {
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

/* Raw-fd sink for shard workers: plain write(), no Python C-API (GIL is
 * released).  Records errno in the context on failure. */
typedef struct { int fd; int err; } wl_rawfd_ctx;

static int
wl_sink_rawfd(void *ctx, const char *buf, size_t len)
{
    wl_rawfd_ctx *c = (wl_rawfd_ctx *) ctx;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(c->fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            c->err = errno ? errno : EIO;
            return -1;
        }
        off += (size_t) n;
    }
    return 0;
}

typedef struct {
    const char *charset;
    Py_ssize_t k;
    const char *delim;
    Py_ssize_t dlen;
    Py_ssize_t minlen;
    char **paths;
    Py_ssize_t nshards;
    Py_ssize_t *first;     /* first work-unit index of each shard */
    Py_ssize_t *last;      /* one past the last work-unit of each shard */
    size_t bufsize;
    size_t maxwidth;
    pthread_mutex_t lock;
    Py_ssize_t next;
    int err;
} wl_sjob;

/*
 * Shard worker: claims whole shards and writes each to its own file.  A
 * work unit is a (length, first-character) pair; unit `u` is length
 * minlen + u/k starting with charset[u % k], i.e. all k^(length-1) words
 * with that length and leading character.  Units are laid out in global
 * output order, so concatenating the shard files in order reproduces the
 * single-file output exactly.  Separate files mean separate inodes, so
 * the writers do not contend on a shared inode lock.
 */
static void *
wl_shard_worker(void *arg)
{
    wl_sjob *job = (wl_sjob *) arg;
    char *out = (char *) malloc(job->bufsize + WL_SLACK);
    char *row = (char *) malloc(job->maxwidth + WL_SLACK);
    Py_ssize_t *varoff = (Py_ssize_t *) malloc(
        (size_t) (job->maxwidth ? job->maxwidth : 1) * sizeof(Py_ssize_t));

    if (out == NULL || row == NULL || varoff == NULL) {
        pthread_mutex_lock(&job->lock);
        if (!job->err) {
            job->err = ENOMEM;
        }
        pthread_mutex_unlock(&job->lock);
        free(out);
        free(row);
        free(varoff);
        return NULL;
    }

    for (;;) {
        Py_ssize_t s, u;
        int fd, rc = 0;
        wl_rawfd_ctx ctx;

        pthread_mutex_lock(&job->lock);
        if (job->err || job->next >= job->nshards) {
            pthread_mutex_unlock(&job->lock);
            break;
        }
        s = job->next++;
        pthread_mutex_unlock(&job->lock);

        fd = open(job->paths[s], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            pthread_mutex_lock(&job->lock);
            if (!job->err) {
                job->err = errno ? errno : EIO;
            }
            pthread_mutex_unlock(&job->lock);
            break;
        }
        ctx.fd = fd;
        ctx.err = 0;

        for (u = job->first[s]; u < job->last[s] && rc == 0; u++) {
            Py_ssize_t L = job->minlen + u / job->k;
            Py_ssize_t c = u % job->k;
            size_t width = (size_t) L + (size_t) job->dlen;
            Py_ssize_t i;

            row[0] = job->charset[c];
            for (i = 1; i < L; i++) {
                row[i] = job->charset[0];
                varoff[i - 1] = i;
            }
            if (job->dlen) {
                memcpy(row + L, job->delim, (size_t) job->dlen);
            }
            memset(row + width, 0, WL_SLACK);

            rc = wl_emit_core(out, job->bufsize, row, width, varoff, L - 1,
                              job->charset, job->k, wl_sink_rawfd, &ctx);
        }
        close(fd);

        if (rc) {
            int e = (rc == -2) ? ENOMEM : (ctx.err ? ctx.err : EIO);
            pthread_mutex_lock(&job->lock);
            if (!job->err) {
                job->err = e;
            }
            pthread_mutex_unlock(&job->lock);
            break;
        }
    }

    free(out);
    free(row);
    free(varoff);
    return NULL;
}

/* Run shard workers across up to nthreads threads (caller thread helps). */
static void
wl_run_shards(wl_sjob *job, int nthreads)
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
        if (pthread_create(&tids[i], NULL, wl_shard_worker, job) == 0) {
            made++;
        }
        else {
            break;
        }
    }
    wl_shard_worker(job);
    for (i = 0; i < made; i++) {
        pthread_join(tids[i], NULL);
    }
    Py_END_ALLOW_THREADS

    free(tids);
}

/*
 * write_words_sharded(fd_unused, charset, delim, minlen, maxlen,
 *                     paths, nthreads)
 *
 * Generate the same bytes as write_words, but partitioned across the
 * files named in `paths` (one per shard) so they can be produced
 * concurrently without inode-lock contention.  Concatenating the files
 * in list order reproduces the single-file output.
 */
static PyObject *
py_write_words_sharded(PyObject *self, PyObject *args)
{
    const char *charset, *delim;
    Py_ssize_t k, dlen, minlen, maxlen;
    PyObject *path_seq;
    int nthreads;
    Py_ssize_t nshards, nunits, u, i;
    char **paths = NULL;
    Py_ssize_t *first = NULL, *last = NULL;
    double *cum = NULL, total;
    wl_sjob job;
    int rc_err;

    if (!PyArg_ParseTuple(args, "y#y#nnOi", &charset, &k, &delim, &dlen,
                          &minlen, &maxlen, &path_seq, &nthreads)) {
        return NULL;
    }
    if (minlen < 1 || maxlen < minlen) {
        PyErr_SetString(PyExc_ValueError, "invalid length range");
        return NULL;
    }
    path_seq = PySequence_Fast(path_seq, "paths must be a sequence");
    if (path_seq == NULL) {
        return NULL;
    }
    nshards = PySequence_Fast_GET_SIZE(path_seq);
    if (nshards < 1) {
        Py_DECREF(path_seq);
        PyErr_SetString(PyExc_ValueError, "need at least one shard path");
        return NULL;
    }
    if (k <= 0) {
        Py_DECREF(path_seq);
        Py_RETURN_NONE;
    }

    paths = (char **) calloc((size_t) nshards, sizeof(char *));
    if (paths == NULL) {
        Py_DECREF(path_seq);
        return PyErr_NoMemory();
    }
    for (i = 0; i < nshards; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(path_seq, i);
        if (!PyBytes_Check(item)) {
            free(paths);
            Py_DECREF(path_seq);
            PyErr_SetString(PyExc_TypeError, "shard paths must be bytes");
            return NULL;
        }
        paths[i] = PyBytes_AsString(item);  /* borrowed, valid while seq held */
    }

    /* Partition the (length, first-char) units into contiguous,
     * roughly byte-balanced shards.  Unit size depends only on length. */
    nunits = (maxlen - minlen + 1) * k;
    cum = (double *) malloc((size_t) (nunits + 1) * sizeof(double));
    first = (Py_ssize_t *) malloc((size_t) nshards * sizeof(Py_ssize_t));
    last = (Py_ssize_t *) malloc((size_t) nshards * sizeof(Py_ssize_t));
    if (cum == NULL || first == NULL || last == NULL) {
        free(paths);
        free(cum);
        free(first);
        free(last);
        Py_DECREF(path_seq);
        return PyErr_NoMemory();
    }
    cum[0] = 0.0;
    for (u = 0; u < nunits; u++) {
        Py_ssize_t L = minlen + u / k;
        double sz = (double) (L + dlen);
        Py_ssize_t e;
        for (e = 1; e < L; e++) {
            sz *= (double) k;          /* k^(L-1) * (L+dlen) */
        }
        cum[u + 1] = cum[u] + sz;
    }
    total = cum[nunits];
    for (i = 0; i < nshards; i++) {
        first[i] = -1;
        last[i] = 0;
    }
    for (u = 0; u < nunits; u++) {
        Py_ssize_t sh = (total > 0.0)
            ? (Py_ssize_t) (cum[u] * (double) nshards / total) : 0;
        if (sh >= nshards) {
            sh = nshards - 1;
        }
        if (first[sh] < 0) {
            first[sh] = u;
        }
        last[sh] = u + 1;
    }
    for (i = 0; i < nshards; i++) {
        if (first[i] < 0) {       /* empty shard: writes an empty file */
            first[i] = 0;
            last[i] = 0;
        }
    }

    job.charset = charset;
    job.k = k;
    job.delim = delim;
    job.dlen = dlen;
    job.minlen = minlen;
    job.paths = paths;
    job.nshards = nshards;
    job.first = first;
    job.last = last;
    job.maxwidth = (size_t) maxlen + (size_t) dlen;
    job.bufsize = wl_bufsize((size_t) k * job.maxwidth);
    job.next = 0;
    job.err = 0;
    if (pthread_mutex_init(&job.lock, NULL) != 0) {
        rc_err = errno ? errno : EAGAIN;
        free(paths);
        free(cum);
        free(first);
        free(last);
        Py_DECREF(path_seq);
        return wl_oserror(rc_err);
    }

    wl_run_shards(&job, nthreads);
    pthread_mutex_destroy(&job.lock);

    rc_err = job.err;
    free(paths);
    free(cum);
    free(first);
    free(last);
    Py_DECREF(path_seq);

    if (rc_err) {
        return wl_oserror(rc_err);
    }
    Py_RETURN_NONE;
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
    {"write_words_sharded", py_write_words_sharded, METH_VARARGS,
     "write_words_sharded(charset, delim, minlen, maxlen, paths, nthreads): "
     "write every word partitioned across the given shard files."},
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
