# -*- coding: utf-8 -*-
############################################
#                                          #
# Wordlist generator, creates dictionaries #
# Developed by rexos.                      #
# Code and performance optimisation by     #
# dbonadiman.                              #
#                                          #
############################################
"""Wordlist

Generates all possible permutations of a given charset.
"""

from __future__ import print_function

import os

from itertools import product

try:
    # Python 2: lazy map, keeps single-word generation out of memory.
    from itertools import imap as _imap
except ImportError:
    # Python 3: map is already lazy.
    _imap = map

import wordlist._util as utils
import wordlist._fast as _fast


# Maximum number of words assembled into a single in-memory block by the
# bulk (block) generators.  Caps the memory used while still letting most
# of the string assembly happen in a handful of C-level ``str.join`` calls
# instead of one call per word.  ~64k words keeps blocks small enough to
# stay cache friendly.
_BLOCK_WORDS = 1 << 16


def _iter_blocks(pools, cap=_BLOCK_WORDS):
    """
    Yield the full cartesian product of ``pools`` (an iterable of
    character pools) as concatenated string blocks rather than one word
    at a time.

    The pools are split into a *head* and a *tail*.  Every tail
    combination is materialised once into a list of strings (one
    ``str.join`` each); then, for each head prefix, an entire block of
    words is produced with a single ``str.join``.  The whole product is
    therefore emitted with only ``tail_count + head_count`` join calls
    instead of one per word.

    Because ``tail_count * head_count`` is fixed (it is the total number
    of words), that sum is smallest when the split is *balanced* — each
    side close to the square root of the total — not when the tail is as
    large as possible.  The split is chosen to minimise the call count,
    subject to ``cap`` bounding the tail (and hence per-block memory).
    """
    sizes = [len(pool) for pool in pools]
    count = len(pools)

    total = 1
    for size in sizes:
        total *= size

    # Pick the split point that minimises tail_count + head_count while
    # keeping the materialised tail within the memory cap.  ``split`` is
    # the index where the tail begins: head = pools[:split], tail =
    # pools[split:].
    best_split = count - 1
    best_cost = None
    tail_count = 1
    for split in range(count - 1, -1, -1):
        tail_count *= sizes[split]
        if tail_count > cap:
            break
        head_count = total // tail_count
        cost = tail_count + head_count
        if best_cost is None or cost < best_cost:
            best_cost = cost
            best_split = split

    head_pools = pools[:best_split]
    tail_pools = pools[best_split:]
    joiner = ''.join

    tail = [joiner(combo) for combo in product(*tail_pools)] if tail_pools \
        else ['']

    # Leading '' so that ``prefix.join(parts)`` puts a prefix in front of
    # every tail word -- i.e. yields prefix+t0 + prefix+t1 + ...  in a
    # single join, avoiding the extra full-block copy that
    # ``prefix + prefix.join(tail)`` would incur.
    parts = [''] + tail

    for head in product(*head_pools):
        yield joiner(head).join(parts)


def _length_pools(charset, length, delimiter):
    """Pools for every word of ``length`` followed by ``delimiter``."""
    # The delimiter is a constant one-element trailing pool, so it is
    # appended by the join itself for empty/single/multi-char delimiters.
    return [charset] * length + [(delimiter,)]


def _pattern_pools(charset, pattern, delimiter):
    """Pools for a pattern: '@' -> charset, anything else -> fixed char."""
    pools = [charset if ch == '@' else (ch,) for ch in pattern]
    pools.append((delimiter,))
    return pools


class Generator(object):
    """
    Wordlist class is the wordlist itself, will do the job
    """
    def __init__(self, charset, delimiter=''):
        self.charset = utils.parse_charset(charset)
        self.delimiter = delimiter

    def generate(self, minlen, maxlen):
        """
        Generates words of different length without storing
        them into memory, enforced by itertools.product

        Yields one word at a time.  The delimiter is folded into the
        product as a constant trailing pool so each word is built by a
        single C-level ``str.join`` with no per-word concatenation.
        """
        if minlen < 1 or maxlen < minlen:
            raise ValueError(
                'length range must satisfy 1 <= minlen <= maxlen '
                '(got minlen=%r, maxlen=%r)' % (minlen, maxlen))

        charset = self.charset
        delimiter = self.delimiter
        joiner = ''.join
        for cur in range(minlen, maxlen + 1):
            pools = _length_pools(charset, cur, delimiter)
            for word in _imap(joiner, product(*pools)):
                yield word

    def generate_blocks(self, minlen, maxlen):
        """
        Same output as :meth:`generate`, but yields large concatenated
        string blocks instead of individual words.  Assembling words in
        bulk lets the heavy lifting happen in a few ``str.join`` calls
        rather than one per word, which is dramatically faster when the
        result is streamed straight to a file or stdout.  Memory stays
        bounded by ``_BLOCK_WORDS``.
        """
        if minlen < 1 or maxlen < minlen:
            raise ValueError(
                'length range must satisfy 1 <= minlen <= maxlen '
                '(got minlen=%r, maxlen=%r)' % (minlen, maxlen))

        charset = self.charset
        delimiter = self.delimiter
        for cur in range(minlen, maxlen + 1):
            for block in _iter_blocks(_length_pools(charset, cur, delimiter)):
                yield block

    def generate_with_pattern(self, pattern=None):
        """
        Algorithm that creates the list
        based on a given pattern
        The pattern must be like string format patter:
        e.g: a@b will match an 'a' follow by any character follow by a 'b'

        Yields one word at a time.  Each pattern position becomes a pool
        for itertools.product (a fixed character is a one-element pool,
        every '@' is the whole charset), and words are assembled by a
        single ``str.join`` instead of ``str.format``.
        """
        if utils.get_pattern_length(pattern) <= 0:
            return

        pools = _pattern_pools(self.charset, pattern, self.delimiter)
        for word in _imap(''.join, product(*pools)):
            yield word

    def generate_with_pattern_blocks(self, pattern=None):
        """
        Same output as :meth:`generate_with_pattern`, but yields large
        concatenated string blocks instead of individual words for fast
        bulk streaming.  Memory stays bounded by ``_BLOCK_WORDS``.
        """
        if utils.get_pattern_length(pattern) <= 0:
            return

        pools = _pattern_pools(self.charset, pattern, self.delimiter)
        for block in _iter_blocks(pools):
            yield block

    def write(self, fileobj, minlen, maxlen):
        """
        Write every word of length ``minlen``..``maxlen`` to ``fileobj``.

        Uses the compiled accelerator to stream the result straight to
        the file descriptor when it is available and the charset/delimiter
        are pure ASCII; otherwise falls back to the portable block
        generator.  Either way the bytes produced are identical.
        """
        if minlen < 1 or maxlen < minlen:
            raise ValueError(
                'length range must satisfy 1 <= minlen <= maxlen '
                '(got minlen=%r, maxlen=%r)' % (minlen, maxlen))
        if not _fast.write_words(fileobj, self.charset, self.delimiter,
                                 minlen, maxlen):
            fileobj.writelines(self.generate_blocks(minlen, maxlen))

    def write_with_pattern(self, fileobj, pattern=None):
        """
        Write every word matching ``pattern`` to ``fileobj``.

        Uses the compiled accelerator when possible (see :meth:`write`),
        falling back to the portable block generator otherwise.
        """
        if utils.get_pattern_length(pattern) <= 0:
            return
        if not _fast.write_pattern(fileobj, self.charset, self.delimiter,
                                   pattern):
            fileobj.writelines(self.generate_with_pattern_blocks(pattern))

    def write_sharded(self, paths, minlen, maxlen, nthreads=None):
        """
        Write every word of length ``minlen``..``maxlen`` partitioned
        across the files named in ``paths`` (one per shard), generated
        concurrently.  Concatenating the files in list order reproduces
        exactly what :meth:`write` would write to a single file.

        Writing to separate files avoids the single-inode write
        bottleneck, so this scales across cores.  Falls back to writing
        the whole output to ``paths[0]`` (leaving the rest empty) when the
        accelerator is unavailable or the charset is not pure ASCII.
        """
        if minlen < 1 or maxlen < minlen:
            raise ValueError(
                'length range must satisfy 1 <= minlen <= maxlen '
                '(got minlen=%r, maxlen=%r)' % (minlen, maxlen))
        if not paths:
            raise ValueError('need at least one shard path')
        if nthreads is None:
            nthreads = min(len(paths), os.cpu_count() or 1)

        if not _fast.write_words_sharded(paths, self.charset, self.delimiter,
                                         minlen, maxlen, nthreads):
            # Portable fallback: everything into the first file, the rest
            # empty.  Still concatenates to the single-file output.
            with open(paths[0], 'wb') as handle:
                for block in self.generate_blocks(minlen, maxlen):
                    handle.write(block.encode())
            for extra in paths[1:]:
                open(extra, 'wb').close()
