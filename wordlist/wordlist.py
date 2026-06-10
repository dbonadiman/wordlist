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

from itertools import product

try:
    # Python 2: lazy map, keeps single-word generation out of memory.
    from itertools import imap as _imap
except ImportError:
    # Python 3: map is already lazy.
    _imap = map

import wordlist._util as utils


# Maximum number of words assembled into a single in-memory block by the
# bulk (block) generators.  Caps the memory used while still letting most
# of the string assembly happen in a handful of C-level ``str.join`` calls
# instead of one call per word.  ~64k words keeps blocks small enough to
# stay cache friendly.
_BLOCK_WORDS = 1 << 16


def _iter_blocks(pools, target=_BLOCK_WORDS):
    """
    Yield the full cartesian product of ``pools`` (an iterable of
    character pools) as concatenated string blocks rather than one word
    at a time.

    The pools are split into a *head* and a *tail*.  Every tail
    combination is materialised once into a list of strings; then, for
    each head prefix, an entire block of words is produced with a single
    ``str.join``.  This collapses what would be ``product`` of all pool
    sizes ``str.join`` calls down to roughly ``head_size + tail_size``
    calls, while ``target`` bounds the tail (and therefore the per-block
    memory).
    """
    sizes = [len(pool) for pool in pools]
    count = len(pools)

    # Grow the tail from the right while it stays within the target size,
    # so the bulk of the work happens in the precomputed tail join.
    split = count
    tail_size = 1
    while split > 0 and tail_size * sizes[split - 1] <= target:
        split -= 1
        tail_size *= sizes[split]
    if split == count:
        # Even a single trailing pool exceeds the target; keep one in the
        # tail so there is always something to join in bulk.
        split = count - 1

    head_pools = pools[:split]
    tail_pools = pools[split:]
    joiner = ''.join

    tail = [joiner(combo) for combo in product(*tail_pools)] if tail_pools \
        else ['']

    for head in product(*head_pools):
        prefix = joiner(head)
        # ''.join(prefix + t for t in tail) without the per-word Python loop.
        yield prefix + prefix.join(tail)


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
            raise ValueError()

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
            raise ValueError()

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
