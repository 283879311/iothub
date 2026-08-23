#!/usr/bin/env python3
import argparse
import pathlib
import re
import sys


SCRIPT_PATTERN = re.compile(r"(?is)<script\b(?P<attrs>[^>]*)>(?P<body>.*?)</script\s*>")
SKIP_SRC_PATTERN = re.compile(r"\bsrc\s*=", re.IGNORECASE)


class SyntaxCheckError(ValueError):
    pass


def iter_inline_scripts(html_text: str):
    for index, match in enumerate(SCRIPT_PATTERN.finditer(html_text), start=1):
        attrs = match.group("attrs") or ""
        if SKIP_SRC_PATTERN.search(attrs):
            continue
        yield index, match.group("body")


def is_regex_start(prev_char: str | None) -> bool:
    return prev_char is None or prev_char in "([{=:+-!*,;?&|^~<>%"


def update_position(char: str, line: int, column: int):
    if char == "\n":
        return line + 1, 1
    return line, column + 1


def check_script_syntax(script: str, label: str):
    stack: list[tuple[str, int, int]] = []
    state = "code"
    prev_significant = None
    string_quote = ""
    regex_in_class = False
    line = 1
    column = 1
    start_line = 1
    start_column = 1
    index = 0
    length = len(script)

    while index < length:
        char = script[index]
        next_char = script[index + 1] if index + 1 < length else ""

        if state == "line_comment":
            if char == "\n":
                state = "code"
            line, column = update_position(char, line, column)
            index += 1
            continue

        if state == "block_comment":
            if char == "*" and next_char == "/":
                line, column = update_position(char, line, column)
                line, column = update_position(next_char, line, column)
                index += 2
                state = "code"
                continue
            line, column = update_position(char, line, column)
            index += 1
            continue

        if state == "string":
            if char == "\\":
                line, column = update_position(char, line, column)
                index += 1
                if index < length:
                    line, column = update_position(script[index], line, column)
                    index += 1
                continue
            if char == "\n":
                raise SyntaxCheckError(f"{label}:{line}:{column}: unterminated string literal")
            if char == string_quote:
                state = "code"
            line, column = update_position(char, line, column)
            index += 1
            continue

        if state == "template":
            if char == "\\":
                line, column = update_position(char, line, column)
                index += 1
                if index < length:
                    line, column = update_position(script[index], line, column)
                    index += 1
                continue
            if char == "`":
                state = "code"
                prev_significant = "`"
                line, column = update_position(char, line, column)
                index += 1
                continue
            if char == "$" and next_char == "{":
                stack.append(("${", line, column))
                state = "code"
                line, column = update_position(char, line, column)
                line, column = update_position(next_char, line, column)
                index += 2
                continue
            line, column = update_position(char, line, column)
            index += 1
            continue

        if state == "regex":
            if char == "\\":
                line, column = update_position(char, line, column)
                index += 1
                if index < length:
                    line, column = update_position(script[index], line, column)
                    index += 1
                continue
            if char == "[":
                regex_in_class = True
            elif char == "]" and regex_in_class:
                regex_in_class = False
            elif char == "/" and not regex_in_class:
                state = "code"
                prev_significant = "/"
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char in " \t\r":
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char == "\n":
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char == "/" and next_char == "/":
            state = "line_comment"
            line, column = update_position(char, line, column)
            line, column = update_position(next_char, line, column)
            index += 2
            continue

        if char == "/" and next_char == "*":
            state = "block_comment"
            start_line, start_column = line, column
            line, column = update_position(char, line, column)
            line, column = update_position(next_char, line, column)
            index += 2
            continue

        if char in ("'", '"'):
            state = "string"
            string_quote = char
            start_line, start_column = line, column
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char == "`":
            state = "template"
            start_line, start_column = line, column
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char == "/" and is_regex_start(prev_significant):
            state = "regex"
            regex_in_class = False
            start_line, start_column = line, column
            line, column = update_position(char, line, column)
            index += 1
            continue

        if char in "([{":
            stack.append((char, line, column))
        elif char in ")]}":
            if not stack:
                raise SyntaxCheckError(f"{label}:{line}:{column}: unexpected '{char}'")
            opener, opener_line, opener_column = stack.pop()
            if char == "}" and opener == "${":
                state = "template"
            elif ((opener == "(" and char != ")")
                  or (opener == "[" and char != "]")
                  or (opener == "{" and char != "}")):
                raise SyntaxCheckError(
                    f"{label}:{line}:{column}: unexpected '{char}', expected match for "
                    f"'{opener}' opened at {opener_line}:{opener_column}"
                )

        prev_significant = char
        line, column = update_position(char, line, column)
        index += 1

    if state == "string":
        raise SyntaxCheckError(f"{label}:{start_line}:{start_column}: unterminated string literal")
    if state == "template":
        raise SyntaxCheckError(f"{label}:{start_line}:{start_column}: unterminated template literal")
    if state == "regex":
        raise SyntaxCheckError(f"{label}:{start_line}:{start_column}: unterminated regular expression literal")
    if state == "block_comment":
        raise SyntaxCheckError(f"{label}:{start_line}:{start_column}: unterminated block comment")
    if stack:
        opener, opener_line, opener_column = stack[-1]
        raise SyntaxCheckError(
            f"{label}:{opener_line}:{opener_column}: unclosed '{opener}'"
        )


def check_html_file(file_path: pathlib.Path):
    html_text = file_path.read_text(encoding="utf-8")
    script_count = 0
    for script_index, script_body in iter_inline_scripts(html_text):
        script_count += 1
        check_script_syntax(script_body, f"{file_path.name}#script{script_index}")
    return script_count


def main():
    parser = argparse.ArgumentParser(description="Check inline JavaScript syntax in HTML files.")
    parser.add_argument("source", type=pathlib.Path, help="Web source directory")
    args = parser.parse_args()

    html_files = sorted(args.source.resolve().glob("*.html"))
    checked_scripts = 0
    for html_file in html_files:
        checked_scripts += check_html_file(html_file)
    print(f"Checked {checked_scripts} inline script block(s) in {len(html_files)} HTML file(s).")


if __name__ == "__main__":
    try:
        main()
    except SyntaxCheckError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
