try:
    from setuptools import setup, Extension
    _HAVE_SETUPTOOLS = True
except ImportError:
    from distutils.core import setup, Extension
    _HAVE_SETUPTOOLS = False

import wordlist

install_requires = []

# Optional C accelerator.  Marked optional so a missing compiler never
# breaks installation -- the package falls back to the pure-Python path.
speedups = Extension(
    'wordlist._speedups',
    sources=['wordlist/_speedups.c'],
    optional=True,
)


config = {
    'description': 'Wordlist generator, creates dictionaries of words',
    'author': 'Rexos',
    'url': 'https://github.com/rexos/wordlist',
    'download_url': 'https://github.com/rexos/wordlist/tarball/1.0',
    'author_email': 'alex.pellegrini@live.com',
    'version': wordlist.__version__,
    'install_requires': install_requires,
    'packages': ['wordlist'],
    'ext_modules': [speedups],
    'scripts': ['bin/wordlist'],
    'keywords': ['words', 'generator', 'wordlist'],
    'name': wordlist.__title__
}

setup(**config)


#TODO: change the download_url
#TODO: change author author_email
