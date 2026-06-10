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

    idx = (Py_ssize_t *) PyMem_Calloc(nvar > 0 ? nvar : 1, sizeof(Py_ssize_t));
    if (idx == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    for (;;) {
        if (outlen + width > bufsize) {
            if (outlen && wl_flush(fd, out, outlen) < 0) {
                rc = -1;
                goto done;
            }
            outlen = 0;
        }
        memcpy(out + outlen, row, width);
        outlen += width;

        /* increment the odometer from the right-most variable position */
        {
            Py_ssize_t pos = nvar - 1;
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
                break;  /* every digit wrapped: product exhausted */
            }
        }
    }

    *outlen_io = outlen;
done:
    PyMem_Free(idx);
    return rc;
}

/* Pick an output buffer large enough for the buffer default and one row. */
static size_t
wl_bufsize(size_t width)
{
    size_t need = width + 64;
    return need > WL_OUTBUF ? need : WL_OUTBUF;
}

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
    bufsize = wl_bufsize(maxwidth);
    out = (char *) PyMem_Malloc(bufsize);
    if (out == NULL) {
        return PyErr_NoMemory();
    }

    for (cur = minlen; cur <= maxlen; cur++) {
        size_t width = (size_t) cur + (size_t) dlen;
        char *row;
        Py_ssize_t *varoff;
        Py_ssize_t i;

        row = (char *) PyMem_Malloc(width > 0 ? width : 1);
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
    row = (char *) PyMem_Malloc(width > 0 ? width : 1);
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

    bufsize = wl_bufsize(width);
    out = (char *) PyMem_Malloc(bufsize);
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

static PyMethodDef wl_methods[] = {
    {"write_words", py_write_words, METH_VARARGS,
     "write_words(fd, charset, delim, minlen, maxlen): stream every word "
     "of the given lengths to fd."},
    {"write_pattern", py_write_pattern, METH_VARARGS,
     "write_pattern(fd, charset, delim, pattern): stream every word "
     "matching pattern to fd."},
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
    return PyModule_Create(&wl_module);
}
