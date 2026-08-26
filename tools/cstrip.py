"""cstrip.py -- strip C comments and string/char literals, preserving line count.

Why this exists as its own file: all three checkers had their own stripper, and
all three were wrong in the same way.

The regex approach cannot work, in either order:

  strings first   the char-literal pattern '(?:[^'\\]|\\.)*' matches newlines
                  inside [^'\\], so an apostrophe in a comment -- "engine's",
                  "does not" -- opens a fake char literal that runs to the next
                  apostrophe, swallowing every newline between. Measured on
                  main.c: 1391 lines in, 772 out. Every reported line number
                  after the first possessive was wrong.

  comments first  a string containing // or /* gets mangled instead.

There is no ordering that fixes both, because the two constructs are mutually
exclusive in a way regex alone cannot express: whether a quote opens a string
depends on not already being in a comment, and vice versa. A single left-to-
right pass tracking one state variable is the smallest correct answer.

Skipped regions are replaced with the same number of newlines they spanned, so
line numbers survive.
"""


def strip(text):
    out = []
    i = 0
    n = len(text)
    CODE, LINE_COMMENT, BLOCK_COMMENT, STRING, CHAR = range(5)
    state = CODE

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == CODE:
            if c == "/" and nxt == "/":
                state = LINE_COMMENT
                i += 2
            elif c == "/" and nxt == "*":
                state = BLOCK_COMMENT
                i += 2
            elif c == '"':
                state = STRING
                out.append('""')          # keep a token so "" != nothing
                i += 1
            elif c == "'":
                state = CHAR
                out.append("''")
                i += 1
            else:
                out.append(c)
                i += 1

        elif state == LINE_COMMENT:
            # A line comment ends at the newline, which is kept.
            if c == "\n":
                out.append("\n")
                state = CODE
            i += 1

        elif state == BLOCK_COMMENT:
            if c == "*" and nxt == "/":
                state = CODE
                i += 2
            else:
                if c == "\n":
                    out.append("\n")      # preserve the line
                i += 1

        elif state in (STRING, CHAR):
            closer = '"' if state == STRING else "'"
            if c == "\\":
                # Escape: consume both characters. A backslash-newline is a
                # line continuation inside the literal; keep the newline.
                if nxt == "\n":
                    out.append("\n")
                i += 2
            elif c == closer:
                state = CODE
                i += 1
            else:
                if c == "\n":
                    # Unterminated literal. Real code should not have one, but
                    # keeping the newline means a mistake here costs accuracy
                    # rather than every line number after it.
                    out.append("\n")
                    state = CODE
                i += 1

    return "".join(out)


def find_unterminated_strings(text):
    """Lines where a string or char literal is still open at the newline.

    C only allows that with a backslash-continuation, so anything else is a
    syntax error the compiler reports as "missing terminating \" character".

    Added after a self-inflicted build break: a re.sub replacement template
    expands \n to a real newline, so writing \\n in the Python source put
    actual line breaks inside three C string literals. Three compile errors
    from one bad escape.

    Uses the same state machine as strip(), because the naive version -- count
    quotes per line -- gets "res://" wrong (it sees // as a comment) and gets
    block comments wrong too. It reported six false positives on this tree
    before being rewritten.
    """
    out = []
    i = 0
    n = len(text)
    line = 1
    CODE, LINE_COMMENT, BLOCK_COMMENT, STRING, CHAR = range(5)
    state = CODE
    opened_at = 0

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if c == "\n":
            if state in (STRING, CHAR):
                out.append((opened_at, text.split("\n")[opened_at - 1].strip()))
                state = CODE          # resync so one error is not reported twice
            elif state == LINE_COMMENT:
                state = CODE
            line += 1
            i += 1
            continue

        if state == CODE:
            if c == "/" and nxt == "/":   state = LINE_COMMENT;  i += 2
            elif c == "/" and nxt == "*": state = BLOCK_COMMENT; i += 2
            elif c == '"':  state = STRING; opened_at = line; i += 1
            elif c == "'":  state = CHAR;   opened_at = line; i += 1
            else: i += 1
        elif state == BLOCK_COMMENT:
            if c == "*" and nxt == "/": state = CODE; i += 2
            else: i += 1
        elif state == LINE_COMMENT:
            i += 1
        else:
            closer = '"' if state == STRING else "'"
            if c == "\\":
                i += 2                    # escape, including backslash-newline
                if nxt == "\n": line += 1
            elif c == closer:
                state = CODE; i += 1
            else:
                i += 1

    return out
