"""Optional C-accelerated output path with a pure-Python fallback.

The functions here try to stream generated words straight to a file
descriptor using the compiled :mod:`wordlist._speedups` extension.  They
return ``True`` when they handled the write and ``False`` when the caller
should fall back to the portable pure-Python path.

The fast path is only taken when the extension is available *and* the
charset, delimiter and pattern are pure ASCII, so it can never change the
bytes that would otherwise be produced.
"""

import os
import stat

try:
    from wordlist import _speedups
    available = True
except Exception:  # pragma: no cover - extension is optional
    _speedups = None
    available = False

# Only spread work across threads once the job is big enough to pay for it.
_PARALLEL_MIN_BYTES = 8 << 20
# Cap the worker count; more threads quickly saturate storage bandwidth.
_PARALLEL_MAX_THREADS = 8

# Escape hatch: set WORDLIST_NO_C=1 to force the pure-Python path even when
# the compiled accelerator is installed (useful for testing/benchmarking).
if os.environ.get('WORDLIST_NO_C'):
    available = False


def _is_ascii(text):
    try:
        return text.isascii()  # Python 3.7+
    except AttributeError:  # pragma: no cover - older interpreters
        try:
            text.encode('ascii')
        except (UnicodeDecodeError, UnicodeEncodeError):
            return False
        return True


def _fileno(fileobj):
    """Return a writable fd for fileobj, or None if it has none."""
    try:
        fd = fileobj.fileno()
    except (AttributeError, OSError, ValueError):
        return None
    return fd


def _parallel_enabled():
    return available and getattr(_speedups, 'has_parallel', 0)


def _nthreads():
    count = os.cpu_count() or 1
    return min(count, _PARALLEL_MAX_THREADS)


def _seekable_regular(fd):
    """True if fd refers to a regular file (so pwrite at offsets is safe)."""
    try:
        return stat.S_ISREG(os.fstat(fd).st_mode)
    except OSError:
        return False


def _est_words_bytes(k, dlen, minlen, maxlen):
    total = 0
    for length in range(minlen, maxlen + 1):
        total += (k ** length) * (length + dlen)
    return total


def _should_parallelize(fd, estimate, nthreads):
    return (_parallel_enabled() and nthreads > 1
            and estimate >= _PARALLEL_MIN_BYTES and _seekable_regular(fd))


def write_words(fileobj, charset, delimiter, minlen, maxlen):
    """Stream every word of length minlen..maxlen to fileobj via C.

    Returns True if handled, False if the caller should fall back.
    """
    if not available:
        return False
    if not (_is_ascii(charset) and _is_ascii(delimiter)):
        return False
    fd = _fileno(fileobj)
    if fd is None:
        return False
    # Flush any buffered Python-level writes so our direct fd writes do
    # not get reordered ahead of them.
    fileobj.flush()
    cset = charset.encode('ascii')
    delim = delimiter.encode('ascii')
    nthreads = _nthreads()
    if _should_parallelize(
            fd, _est_words_bytes(len(cset), len(delim), minlen, maxlen),
            nthreads):
        try:
            _speedups.write_words_parallel(fd, cset, delim, minlen, maxlen,
                                           nthreads)
            return True
        except OverflowError:
            pass  # offsets too large; fall back to the serial path
    _speedups.write_words(fd, cset, delim, minlen, maxlen)
    return True


def write_pattern(fileobj, charset, delimiter, pattern):
    """Stream every word matching pattern to fileobj via C.

    Returns True if handled, False if the caller should fall back.
    """
    if not available:
        return False
    if not (_is_ascii(charset) and _is_ascii(delimiter) and _is_ascii(pattern)):
        return False
    fd = _fileno(fileobj)
    if fd is None:
        return False
    fileobj.flush()
    cset = charset.encode('ascii')
    delim = delimiter.encode('ascii')
    pat = pattern.encode('ascii')
    nthreads = _nthreads()
    estimate = (len(cset) ** pat.count(b'@')) * (len(pat) + len(delim))
    if _should_parallelize(fd, estimate, nthreads):
        try:
            _speedups.write_pattern_parallel(fd, cset, delim, pat, nthreads)
            return True
        except OverflowError:
            pass  # offsets too large; fall back to the serial path
    _speedups.write_pattern(fd, cset, delim, pat)
    return True
