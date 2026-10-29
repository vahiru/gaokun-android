"""Small CIL reader for audited offline transformations, not a policy compiler."""
import re

TOKEN = re.compile(r';[^\n]*|"(?:\\.|[^"\\])*"|[()]|[^\s();"]+')


def tokens(text):
    end = 0
    for match in TOKEN.finditer(text):
        if text[end:match.start()].strip():
            raise ValueError(f"invalid CIL text at {end}")
        end = match.end()
        if not match[0].startswith(';'):
            yield match
    if text[end:].strip():
        raise ValueError(f"invalid trailing CIL text at {end}")


def statements(text):
    """Return (start, end, tree) for each top-level expression; reject malformed CIL."""
    stack = []
    start = 0
    result = []
    for match in tokens(text):
        token = match[0]
        if token == '(':
            if not stack:
                start = match.start()
            stack.append([])
        elif token == ')':
            if not stack:
                raise ValueError("unbalanced CIL closing parenthesis")
            tree = stack.pop()
            if not tree and not stack:
                raise ValueError("empty CIL expression")
            if stack:
                stack[-1].append(tree)
            else:
                result.append((start, match.end(), tree))
        else:
            if not stack:
                raise ValueError("CIL atom outside expression")
            stack[-1].append(token)
    if stack:
        raise ValueError("unterminated CIL expression")
    return result


def replace_symbols(text, mapping):
    result, end = [], 0
    for match in tokens(text):
        result += [text[end:match.start()], mapping.get(match[0], match[0])]
        end = match.end()
    result.append(text[end:])
    return ''.join(result)
