import shlex


def shlex_join(arglist) -> str:
    #
    # Implementation for Python 3.7 and older
    # https://bugs.python.org/issue22454
    #
    return " ".join([shlex.quote(x) for x in arglist])
