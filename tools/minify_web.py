#!/usr/bin/env python3
import argparse
import pathlib
import re
import shutil


RAW_TAGS = ("script", "style", "pre", "textarea")
COMMENT_PATTERN = re.compile(r"<!--(?!\[if\b)[\s\S]*?-->", re.IGNORECASE)
RAW_BLOCK_PATTERN = re.compile(
    r"(?is)<(?P<tag>script|style|pre|textarea)\b[^>]*>.*?</(?P=tag)\s*>"
)
TAG_GAP_PATTERN = re.compile(r">\s+<")
TEXT_WS_PATTERN = re.compile(r"\s+")
CSS_COMMENT_PATTERN = re.compile(r"/\*[\s\S]*?\*/")
CSS_SPACE_AROUND_PATTERN = re.compile(r"\s*([{}:;,>+~])\s*")
HTML_BLOCK_PATTERN = re.compile(
    r"(?is)(<(?P<tag>script|style|pre|textarea)\b[^>]*>)(?P<body>.*?)(</(?P=tag)\s*>)"
)


def is_alphanum(char: str) -> bool:
    return bool(char) and (char.isalnum() or char in "_$\\")


class JsMinifier:
    def __init__(self, script: str):
        self.script = script
        self.index = 0
        self.length = len(script)
        self.lookahead = ""

    def _get(self) -> str:
        if self.lookahead:
            char = self.lookahead
            self.lookahead = ""
            return char
        if self.index >= self.length:
            return ""
        char = self.script[self.index]
        self.index += 1
        if char >= " " or char == "\n":
            return char
        return " "

    def _peek(self) -> str:
        self.lookahead = self._get()
        return self.lookahead

    def _next(self) -> str:
        char = self._get()
        if char != "/":
            return char
        peek = self._peek()
        if peek == "/":
            while True:
                char = self._get()
                if char in ("\n", ""):
                    return char
        if peek == "*":
            self._get()
            while True:
                char = self._get()
                if char == "":
                    raise ValueError("Unterminated comment in script")
                if char == "*" and self._peek() == "/":
                    self._get()
                    return " "
        return char

    def minify(self) -> str:
        action = 3
        output = []
        char_a = "\n"
        char_b = ""

        while True:
            if action <= 1:
                if char_a:
                    output.append(char_a)
            if action <= 2:
                char_a = char_b
                if char_a in ("'", '"', "`"):
                    quote = char_a
                    while True:
                        output.append(char_a)
                        char_a = self._get()
                        if char_a == quote:
                            break
                        if char_a == "\\":
                            output.append(char_a)
                            char_a = self._get()
                        if char_a == "":
                            raise ValueError("Unterminated string literal in script")
            if action <= 3:
                char_b = self._next()
                if char_b == "/" and char_a in "(,=:[!&|?{};\n":
                    output.append(char_a)
                    output.append(char_b)
                    while True:
                        char_a = self._get()
                        if char_a == "/":
                            break
                        if char_a == "\\":
                            output.append(char_a)
                            char_a = self._get()
                        if char_a == "":
                            raise ValueError("Unterminated regular expression literal in script")
                        output.append(char_a)
                    char_b = self._next()
            if char_a == " ":
                action = 1 if is_alphanum(char_b) else 2
            elif char_a == "\n":
                if char_b in "{[(+-!~":
                    action = 1
                elif char_b == " ":
                    action = 3
                else:
                    action = 1 if is_alphanum(char_b) else 2
            elif char_b == " ":
                action = 1 if is_alphanum(char_a) else 3
            elif char_b == "\n":
                if char_a in "}])+-\"'`":
                    action = 1
                elif char_a == " ":
                    action = 3
                else:
                    action = 1 if is_alphanum(char_a) else 3
            else:
                action = 1
            if not char_a and not char_b:
                break

        return "".join(output).strip()


def minify_css(content: str) -> str:
    content = CSS_COMMENT_PATTERN.sub("", content)
    content = TEXT_WS_PATTERN.sub(" ", content)
    content = CSS_SPACE_AROUND_PATTERN.sub(r"\1", content)
    content = re.sub(r";}", "}", content)
    return content.strip()


def minify_script(content: str) -> str:
    return JsMinifier(content).minify()


def minify_raw_block(open_tag: str, tag_name: str, body: str, close_tag: str) -> str:
    tag_name = tag_name.lower()
    if tag_name == "style":
        minified_body = minify_css(body)
    elif tag_name == "script":
        minified_body = minify_script(body)
    else:
        return f"{open_tag}{body}{close_tag}"
    return f"{open_tag}{minified_body}{close_tag}"


def minify_html(content: str) -> str:
    content = COMMENT_PATTERN.sub("", content)
    parts = []
    cursor = 0

    for match in HTML_BLOCK_PATTERN.finditer(content):
        html_chunk = content[cursor:match.start()]
        html_chunk = TEXT_WS_PATTERN.sub(" ", html_chunk)
        html_chunk = TAG_GAP_PATTERN.sub("><", html_chunk)
        parts.append(html_chunk.strip())
        parts.append(
            minify_raw_block(
                match.group(1),
                match.group("tag"),
                match.group("body"),
                match.group(4),
            )
        )
        cursor = match.end()

    tail = content[cursor:]
    tail = TEXT_WS_PATTERN.sub(" ", tail)
    tail = TAG_GAP_PATTERN.sub("><", tail)
    parts.append(tail.strip())

    joined = "".join(part for part in parts if part)
    return joined + "\n"


def process_directory(source_dir: pathlib.Path, output_dir: pathlib.Path) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for source_path in source_dir.rglob("*"):
        relative_path = source_path.relative_to(source_dir)
        target_path = output_dir / relative_path
        if source_path.is_dir():
            target_path.mkdir(parents=True, exist_ok=True)
            continue

        target_path.parent.mkdir(parents=True, exist_ok=True)
        if source_path.suffix.lower() == ".html":
            content = source_path.read_text(encoding="utf-8")
            target_path.write_text(minify_html(content), encoding="utf-8")
        else:
            shutil.copy2(source_path, target_path)


def main() -> None:
    parser = argparse.ArgumentParser(description="Minify web assets into a build directory.")
    parser.add_argument("source", type=pathlib.Path, help="Source web directory")
    parser.add_argument("output", type=pathlib.Path, help="Output directory")
    args = parser.parse_args()

    process_directory(args.source.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
