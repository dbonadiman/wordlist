"""Optional C-accelerated output path with a pure-Python fallback.

The functions here try to stream generated words straight to a file
descriptor using the compiled :mod:`wordlist._speedups` extension.  They
return ``True`` when they handled the write and ``False`` when the caller
should fall back to the portable pure-Python path.

The fast path is only taken when the extension is available *and* the
charset, delimiter and pattern are pure ASCII, so it can never change the
bytes that would otherwise be produced.

Note on threads: the extension also exposes ``write_words_parallel`` /
``write_pattern_parallel`` (POSIX only).  They are not used here: the
serial generator builds output by memcpy doubling at near memory
bandwidth (~20+ GB/s), far faster than the kernel can copy one file's
data into the page cache (~3-4 GB/s).  Single-file output is therefore
write-bound, and splitting it across threads writing the same file only
adds inode-lock contention (measured ~0.85x of serial).  The parallel
entry points remain available for callers that can avoid that contention
(separate files, O_DIRECT).
"""

import os

try:
    from wordlist import _speedups
    available = True
except Exception:  # pragma: no cover - extension is optional
    _speedups = None
    available = False

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
    _speedups.write_words(fd, charset.encode('ascii'),
                          delimiter.encode('ascii'), minlen, maxlen)
    return True


def sharded_supported():
    """True if the C sharded writer is available."""
    return available and hasattr(_speedups, 'write_words_sharded')


def write_words_sharded(paths, charset, delimiter, minlen, maxlen, nthreads):
    """Generate words partitioned across the files named in `paths`.

    Returns True if handled by the accelerator, False to fall back.
    """
    if not sharded_supported():
        return False
    if not (_is_ascii(charset) and _is_ascii(delimiter)):
        return False
    path_bytes = [os.fsencode(p) for p in paths]
    _speedups.write_words_sharded(charset.encode('ascii'),
                                  delimiter.encode('ascii'),
                                  minlen, maxlen, path_bytes, nthreads)
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
    _speedups.write_pattern(fd, charset.encode('ascii'),
                            delimiter.encode('ascii'), pattern.encode('ascii'))
    return True
