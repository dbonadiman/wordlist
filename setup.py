from setuptools import setup

import wordlist


setup(
    name=wordlist.__title__,
    version=wordlist.__version__,
    description="Wordlist generator, creates dictionaries of words",
    author="Rexos",
    author_email="alex.pellegrini@live.com",
    url="https://github.com/rexos/wordlist",
    download_url="https://github.com/rexos/wordlist/tarball/1.1.0",
    packages=["wordlist"],
    scripts=["bin/wordlist"],
    keywords=["words", "generator", "wordlist"],
)
