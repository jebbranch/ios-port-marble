"""Extract exact C function definitions for host checks of prepared sources."""
import re

def function(source, name):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match.group()), source, flags=re.S)
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", masked):
        position = masked.index("(", match.start()) + 1
        depth = 1
        while depth:
            depth += (masked[position] == "(") - (masked[position] == ")")
            position += 1
        while masked[position].isspace(): position += 1
        if masked[position] != "{": continue
        start = source.rfind("\n", 0, match.start()) + 1
        if not source[start:match.start()].strip(): start = source.rfind("\n", 0, start - 1) + 1
        end = position + 1
        depth = 1
        while depth:
            depth += (masked[end] == "{") - (masked[end] == "}")
            end += 1
        return source[start:end] + "\n"
    raise ValueError(f"function definition not found: {name}")
