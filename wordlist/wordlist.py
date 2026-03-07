"""Core generator API for the wordlist package."""

from __future__ import annotations

from itertools import product
from typing import Iterator

import wordlist._util as utils


class Generator:
    """Generate all permutations for a given charset and length/pattern."""

    def __init__(self, charset: str, delimiter: str = "") -> None:
        self.charset = utils.parse_charset(charset)
        self.delimiter = delimiter

    def generate(self, minlen: int, maxlen: int) -> Iterator[str]:
        """Yield all generated words whose length is between *minlen* and *maxlen*."""
        if minlen < 1:
            raise ValueError("minlen must be >= 1")
        if maxlen < minlen:
            raise ValueError("maxlen must be >= minlen")

        for cur in range(minlen, maxlen + 1):
            for each in product(self.charset, repeat=cur):
                yield "".join(each) + self.delimiter

    def generate_with_pattern(self, pattern: str | None = None) -> Iterator[str]:
        """Yield all generated words that match *pattern*.

        ``@`` placeholders are replaced by characters from the configured charset.
        """
        placeholder_count = utils.get_pattern_length(pattern)
        if placeholder_count == 0:
            return

        pattern = (pattern or "") + self.delimiter
        fstring = utils.pattern_to_fstring(pattern)
        for each in product(self.charset, repeat=placeholder_count):
            yield fstring.format(*each)
