import pytest

import wordlist


def test_generate():
    gen = wordlist.Generator("ab")
    assert list(gen.generate(1, 2)) == ["a", "b", "aa", "ab", "ba", "bb"]


def test_generate_fixed_len():
    gen = wordlist.Generator("ab")
    assert list(gen.generate(2, 2)) == ["aa", "ab", "ba", "bb"]


def test_generate_rejects_invalid_lengths():
    gen = wordlist.Generator("ab")

    with pytest.raises(ValueError, match="minlen"):
        next(gen.generate(0, 1))

    with pytest.raises(ValueError, match="maxlen"):
        next(gen.generate(3, 1))


def test_generate_charset_ranges():
    gen = wordlist.Generator("a-b")
    assert list(gen.generate(1, 2)) == ["a", "b", "aa", "ab", "ba", "bb"]


def test_generate_charset_mixed_literals_and_ranges():
    gen = wordlist.Generator("a-cZ")
    assert list(gen.generate(1, 1)) == ["a", "b", "c", "Z"]


def test_generate_with_pattern():
    gen = wordlist.Generator("ab")
    assert list(gen.generate_with_pattern("@@")) == ["aa", "ab", "ba", "bb"]


def test_generate_with_pattern_left_fixed():
    gen = wordlist.Generator("ab")
    assert list(gen.generate_with_pattern("@a")) == ["aa", "ba"]


def test_generate_with_pattern_right_fixed():
    gen = wordlist.Generator("ab")
    assert list(gen.generate_with_pattern("a@")) == ["aa", "ab"]


def test_generate_with_pattern_both_fixed():
    gen = wordlist.Generator("ab")
    assert list(gen.generate_with_pattern("a@b")) == ["aab", "abb"]


def test_generate_with_pattern_empty_pattern():
    gen = wordlist.Generator("ab")
    assert list(gen.generate_with_pattern("")) == []
