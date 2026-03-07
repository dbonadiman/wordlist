"""Utility helpers for :mod:`wordlist`."""

from __future__ import annotations


def char_range(starting_char: str, ending_char: str):
    """Yield characters from *starting_char* to *ending_char* (inclusive)."""
    if not isinstance(starting_char, str) or not isinstance(ending_char, str):
        raise TypeError("starting_char and ending_char must be strings")
    if len(starting_char) != 1 or len(ending_char) != 1:
        raise ValueError("starting_char and ending_char must be a single character")

    start = ord(starting_char)
    stop = ord(ending_char)
    step = 1 if start <= stop else -1
    for codepoint in range(start, stop + step, step):
        yield chr(codepoint)


def parse_charset(charset: str) -> str:
    """Expand range expressions (for example ``a-z``) in *charset*.

    Any standalone characters are kept as-is.
    """
    if not isinstance(charset, str):
        raise TypeError("charset must be a string")

    chars: list[str] = []
    i = 0
    while i < len(charset):
        if i + 2 < len(charset) and charset[i + 1] == "-":
            chars.extend(char_range(charset[i], charset[i + 2]))
            i += 3
            continue

        chars.append(charset[i])
        i += 1

    return "".join(chars)


def get_pattern_length(string: str | None) -> int:
    """Return how many substitution markers (``@``) are in *string*."""
    if not string:
        return 0
    return string.count("@")


def pattern_to_fstring(string: str) -> str:
    """Convert the ``@`` placeholder syntax to a format-string syntax."""
    if not isinstance(string, str):
        raise TypeError("string must be a str")
    return string.replace("@", "{}")
