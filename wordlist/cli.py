"""Console entry point for wordlist."""

from __future__ import annotations

import sys
from argparse import ArgumentParser
from typing import Sequence

import wordlist


def _get_parser() -> ArgumentParser:
    parser = ArgumentParser(description="Generate permutations for a character set")
    parser.add_argument("charset", help="charset or ranges to use (e.g. a-z0-9)")
    parser.add_argument("pattern", nargs="?", help="pattern to follow, using @ as placeholders")
    parser.add_argument("-m", "--min", dest="minlen", type=int, default=1, help="minimum word size")
    parser.add_argument("-M", "--max", dest="maxlen", type=int, help="maximum word size")
    parser.add_argument("-o", "--out", dest="outfile", help="save output to specified file")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _get_parser()
    args = parser.parse_args(argv)

    maxlen = args.maxlen if args.maxlen is not None else len(args.charset)
    generator = wordlist.Generator(args.charset, delimiter="\n")

    stream = sys.stdout if args.outfile is None else open(args.outfile, "w", encoding="utf-8")
    try:
        if args.pattern:
            stream.writelines(generator.generate_with_pattern(args.pattern))
        else:
            stream.writelines(generator.generate(args.minlen, maxlen))
    finally:
        if stream is not sys.stdout:
            stream.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
