""".util
is a private module containing utility
used for the actual implementation of
the wordlist generator
"""

from collections import OrderedDict


def char_range(starting_char, ending_char):
    """
    Create a range generator for chars
    """
    assert isinstance(starting_char, str), 'char_range: Wrong argument/s type'
    assert isinstance(ending_char, str), 'char_range: Wrong argument/s type'

    for char in range(ord(starting_char), ord(ending_char) + 1):
        yield chr(char)


def _is_word_char(char):
    """Mirror regex ``\\w``: ASCII letters, digits and underscore."""
    return char.isalnum() or char == '_'


def parse_charset(charset):
    """
    Expand ``a-z`` style ranges in place while preserving every other
    character.  ``a-z0-9!`` yields the full lower-case alphabet, the
    digits, and ``!``.  A ``-`` that is not part of a valid ascending
    range (e.g. leading, trailing, or ``z-a``) is kept literally.
    """
    result = []
    i = 0
    length = len(charset)
    while i < length:
        if (i + 2 < length and charset[i + 1] == '-'
                and _is_word_char(charset[i]) and _is_word_char(charset[i + 2])
                and ord(charset[i]) <= ord(charset[i + 2])):
            for char in char_range(charset[i], charset[i + 2]):
                result.append(char)
            i += 3
        else:
            result.append(charset[i])
            i += 1
    return ''.join(result)


def get_pattern_length(string):
    """
    Determines the number of characters to be filled in the pattern
    """
    return string.count("@")


def pattern_to_fstring(string):
    """
    Determines the number of characters to be filled in the pattern
    """
    return string.replace("@", "{}")