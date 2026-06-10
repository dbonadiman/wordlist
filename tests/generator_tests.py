from nose.tools import assert_equals, assert_raises
import os
import tempfile

import wordlist


def _write_to_file(method, *args):
    """Run a Generator.write* method against a temp file and return its text."""
    fd, path = tempfile.mkstemp()
    try:
        with os.fdopen(fd, 'w') as handle:
            method(handle, *args)
        with open(path) as handle:
            return handle.read()
    finally:
        os.remove(path)


def test_generate():
    print("testing wordlist.Generator(\"ab\").generate(1, 2)")
    gen = wordlist.Generator("ab")
    c = gen.generate(1, 2)
    assert_equals(next(c), 'a')
    assert_equals(next(c), 'b')
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ab')
    assert_equals(next(c), 'ba')
    assert_equals(next(c), 'bb')
    with assert_raises(StopIteration):
        next(c)


def test_generate_2():
    print("testing wordlist.Generator(\"ab\").generate(2, 2)")
    gen = wordlist.Generator("ab")
    c = gen.generate(2, 2)
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ab')
    assert_equals(next(c), 'ba')
    assert_equals(next(c), 'bb')
    with assert_raises(StopIteration):
        next(c)


def test_generate_3():
    print("testing wordlist.Generator(\"ab\").generate(0, 0)")
    gen = wordlist.Generator("ab")
    with assert_raises(ValueError) as e:
        c = gen.generate(0, 0)
        next(c)
        assert_equals(e.mess, "minlen must be > 0")

    with assert_raises(ValueError):
        c = gen.generate(0, 0)
        next(c)

def test_generate_4():
    print("testing wordlist.Generator(\"ab\").generate(1, 2)")
    gen = wordlist.Generator("a-b")
    c = gen.generate(1, 2)
    assert_equals(next(c), 'a')
    assert_equals(next(c), 'b')
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ab')
    assert_equals(next(c), 'ba')
    assert_equals(next(c), 'bb')
    with assert_raises(StopIteration):
        next(c)


def test_generate_with_pattern():
    print("testing wordlist.Generator(\"ab\").generate_with_pattern(\"@@\")")
    gen = wordlist.Generator("ab")
    c = gen.generate_with_pattern('@@')
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ab')
    assert_equals(next(c), 'ba')
    assert_equals(next(c), 'bb')
    with assert_raises(StopIteration):
        next(c)


def test_generate_with_pattern_2():
    print("testing wordlist.Generator(\"ab\").generate_with_pattern(\"@a\")")
    gen = wordlist.Generator("ab")
    c = gen.generate_with_pattern('@a')
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ba')
    with assert_raises(StopIteration):
        next(c)


def test_generate_with_pattern_3():
    print("testing wordlist.Generator(\"ab\").generate_with_pattern(\"a@\")")
    gen = wordlist.Generator("ab")
    c = gen.generate_with_pattern('a@')
    assert_equals(next(c), 'aa')
    assert_equals(next(c), 'ab')
    with assert_raises(StopIteration):
        next(c)


def test_generate_with_pattern_4():
    print("testing wordlist.Generator(\"ab\").generate_with_pattern(\"a@b\")")
    gen = wordlist.Generator("ab")
    c = gen.generate_with_pattern('a@b')
    assert_equals(next(c), 'aab')
    assert_equals(next(c), 'abb')
    with assert_raises(StopIteration):
        next(c)


def test_generate_with_pattern_5():
    print("testing wordlist.Generator(\"ab\").generate_with_pattern(\"\")")
    gen = wordlist.Generator("ab")
    c = gen.generate_with_pattern('')
    with assert_raises(StopIteration):
        next(c)


def test_write_matches_generate():
    print("testing wordlist.Generator.write matches generate")
    for charset, delim, minlen, maxlen in [
            ("ab", "\n", 1, 2),
            ("abc", "\n", 1, 4),
            ("a-z0-9", "\n", 1, 2),
            ("xyz", "||", 2, 3),
    ]:
        expected = ''.join(
            wordlist.Generator(charset, delim).generate(minlen, maxlen))
        written = _write_to_file(
            wordlist.Generator(charset, delim).write, minlen, maxlen)
        assert_equals(written, expected)


def test_write_sharded_concatenates_to_generate():
    print("testing wordlist.Generator.write_sharded concatenation")
    import shutil
    for charset, delim, minlen, maxlen, nshards in [
            ("ab", "\n", 1, 2, 1),
            ("abc", "\n", 1, 4, 3),
            ("abcde", "\n", 1, 4, 4),
            ("xyz", "||", 2, 3, 5),
            ("a", "\n", 1, 4, 2),
    ]:
        expected = ''.join(
            wordlist.Generator(charset, delim).generate(minlen, maxlen))
        tmpdir = tempfile.mkdtemp()
        try:
            paths = [os.path.join(tmpdir, 'sh.%03d' % i)
                     for i in range(nshards)]
            wordlist.Generator(charset, delim).write_sharded(
                paths, minlen, maxlen)
            joined = ''.join(open(p).read() for p in paths)
            assert_equals(joined, expected)
        finally:
            shutil.rmtree(tmpdir)


def test_write_with_pattern_matches_generate():
    print("testing wordlist.Generator.write_with_pattern matches generate")
    for charset, delim, pattern in [
            ("ab", "\n", "@@"),
            ("ab", "", "a@b"),
            ("abcdef", "\n", "@@x@@"),
            ("0123", "\n", "@-@-@"),
    ]:
        expected = ''.join(
            wordlist.Generator(charset, delim).generate_with_pattern(pattern))
        written = _write_to_file(
            wordlist.Generator(charset, delim).write_with_pattern, pattern)
        assert_equals(written, expected)


def test_write_empty_pattern_writes_nothing():
    print("testing wordlist.Generator.write_with_pattern('') writes nothing")
    written = _write_to_file(wordlist.Generator("ab").write_with_pattern, '')
    assert_equals(written, '')


def _parallel_speedups():
    """Return the _speedups module if it offers the parallel path, else None."""
    try:
        from wordlist import _speedups
    except ImportError:
        return None
    if not getattr(_speedups, 'has_parallel', 0):
        return None
    return _speedups


def _run_parallel(call_name, charset, delim, *rest):
    speedups = _parallel_speedups()
    fd, path = tempfile.mkstemp()
    try:
        with os.fdopen(fd, 'wb') as handle:
            getattr(speedups, call_name)(
                handle.fileno(), charset.encode('ascii'),
                delim.encode('ascii'), *rest)
        with open(path) as handle:
            return handle.read()
    finally:
        os.remove(path)


def test_parallel_words_match_serial():
    print("testing _speedups.write_words_parallel matches generate")
    if _parallel_speedups() is None:
        return  # extension absent or parallel path unsupported on this platform
    for charset, delim, minlen, maxlen in [
            ("ab", "\n", 1, 2),
            ("abcde", "\n", 1, 3),
            ("xyz", "||", 2, 3),
            ("a", "\n", 1, 4),
    ]:
        for nthreads in (1, 2, 3, 4):
            expected = ''.join(
                wordlist.Generator(charset, delim).generate(minlen, maxlen))
            written = _run_parallel(
                'write_words_parallel', charset, delim, minlen, maxlen,
                nthreads)
            assert_equals(written, expected)


def test_parallel_pattern_match_serial():
    print("testing _speedups.write_pattern_parallel matches generate")
    if _parallel_speedups() is None:
        return
    for charset, delim, pattern in [
            ("ab", "\n", "@@"),
            ("abcdef", "\n", "@@x@@"),
            ("0123", "\n", "@-@-@"),
    ]:
        for nthreads in (1, 2, 4):
            expected = ''.join(
                wordlist.Generator(charset, delim).generate_with_pattern(
                    pattern))
            written = _run_parallel(
                'write_pattern_parallel', charset, delim,
                pattern.encode('ascii'), nthreads)
            assert_equals(written, expected)
