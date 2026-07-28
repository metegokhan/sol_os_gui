#!/usr/bin/env python3
"""Generate the package-gated SolarOS manual registry and release catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import tomllib


ID_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]*$")
FRONT_MATTER_DELIMITER = "+++"
QUICK_REFERENCE_HEADING = "quick reference"
SECTION_INFO = {
    "concept": (10, "Getting started"),
    "shell": (20, "Shell and storage"),
    "app": (30, "Applications"),
    "job": (40, "Background jobs"),
    "network": (50, "Networking and security"),
    "hardware": (60, "Hardware and expansion"),
    "api": (70, "Scripting APIs"),
    "service": (80, "System services"),
    "build": (90, "Boards and firmware"),
}


def package_macro(package: str) -> str:
    return "SOLAR_OS_PACKAGE_" + re.sub(r"[^A-Za-z0-9]", "_", package).upper()


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def parse_front_matter(path: Path) -> tuple[dict[str, object], str]:
    source = path.read_text(encoding="utf-8")
    lines = source.splitlines()
    if not lines or lines[0].strip() != FRONT_MATTER_DELIMITER:
        raise ValueError(f"{path}: missing opening {FRONT_MATTER_DELIMITER}")
    try:
        end = next(
            index
            for index, line in enumerate(lines[1:], start=1)
            if line.strip() == FRONT_MATTER_DELIMITER
        )
    except StopIteration as exc:
        raise ValueError(
            f"{path}: missing closing {FRONT_MATTER_DELIMITER}"
        ) from exc
    metadata = tomllib.loads("\n".join(lines[1:end]))
    markdown = "\n".join(lines[end + 1 :]).strip()
    if not markdown:
        raise ValueError(f"{path}: Markdown body is empty")
    return metadata, markdown + "\n"


def heading_text(line: str) -> tuple[int, str] | None:
    match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
    if match is None:
        return None
    return len(match.group(1)), match.group(2).strip()


def extract_quick_reference(markdown: str, path: Path) -> str:
    lines = markdown.splitlines()
    start = None
    level = 0
    for index, line in enumerate(lines):
        heading = heading_text(line)
        if heading is None or heading[1].casefold() != QUICK_REFERENCE_HEADING:
            continue
        start = index + 1
        level = heading[0]
        break
    if start is None:
        raise ValueError(f"{path}: missing Quick reference heading")

    end = len(lines)
    for index in range(start, len(lines)):
        heading = heading_text(lines[index])
        if heading is not None and heading[0] <= level:
            end = index
            break
    reference = "\n".join(lines[start:end]).strip()
    if not reference:
        raise ValueError(f"{path}: Quick reference section is empty")
    return reference


def strip_inline_markdown(text: str) -> str:
    text = re.sub(r"!\[([^]]*)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"`([^`]+)`", r"\1", text)
    text = text.replace("**", "").replace("__", "")
    text = text.replace("*", "")
    return text.strip()


def markdown_to_terminal_text(markdown: str) -> str:
    output: list[str] = []
    paragraph: list[str] = []
    fenced = False

    def flush_paragraph() -> None:
        if not paragraph:
            return
        output.append(" ".join(part.strip() for part in paragraph).strip())
        paragraph.clear()

    for raw in markdown.splitlines():
        line = raw.rstrip()
        if line.startswith("```"):
            flush_paragraph()
            fenced = not fenced
            if not fenced:
                output.append("")
            continue
        if fenced:
            output.append("  " + line)
            continue

        heading = heading_text(line)
        if heading is not None:
            flush_paragraph()
            if output and output[-1] != "":
                output.append("")
            output.append(strip_inline_markdown(heading[1]).upper())
            output.append("")
            continue

        stripped = line.strip()
        if not stripped:
            flush_paragraph()
            if output and output[-1] != "":
                output.append("")
            continue
        if stripped == "---":
            flush_paragraph()
            output.extend(("--------------------------------", ""))
            continue
        if re.match(r"^[-*+]\s+", stripped):
            flush_paragraph()
            output.append("- " + strip_inline_markdown(stripped[2:]))
            continue
        numbered = re.match(r"^(\d+)[.)]\s+(.+)$", stripped)
        if numbered is not None:
            flush_paragraph()
            output.append(
                f"{numbered.group(1)}. {strip_inline_markdown(numbered.group(2))}"
            )
            continue
        if stripped.startswith(">"):
            flush_paragraph()
            output.append("  " + strip_inline_markdown(stripped[1:].lstrip()))
            continue
        paragraph.append(strip_inline_markdown(stripped))

    flush_paragraph()
    while output and output[-1] == "":
        output.pop()
    return "\n".join(output) + "\n"


def load_pages(source: Path, packages_path: Path) -> list[dict[str, object]]:
    if not source.is_dir():
        raise ValueError("manual input must be a directory of Markdown pages")
    package_document = tomllib.loads(packages_path.read_text(encoding="utf-8"))
    known_packages = set(package_document.get("packages", {}))

    pages: list[dict[str, object]] = []
    seen_ids: set[str] = set()
    for path in sorted(source.glob("*.md")):
        if path.name.casefold() == "readme.md":
            continue
        metadata, markdown = parse_front_matter(path)
        page = dict(metadata)
        for field in ("id", "title", "section", "summary", "keywords"):
            value = page.get(field)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{path}: invalid {field}")
            page[field] = value.strip()

        page_id = str(page["id"])
        if not ID_RE.fullmatch(page_id):
            raise ValueError(f"invalid manual page id: {page_id}")
        if path.stem != page_id:
            raise ValueError(
                f"{path}: filename must match manual page id {page_id}"
            )
        if page_id in seen_ids:
            raise ValueError(f"duplicate manual page id: {page_id}")
        seen_ids.add(page_id)

        aliases = page.get("aliases", [])
        if not isinstance(aliases, list) or not all(
            isinstance(alias, str) and ID_RE.fullmatch(alias.strip())
            for alias in aliases
        ):
            raise ValueError(f"{page_id}: aliases must be valid topic names")
        page["aliases"] = [alias.strip() for alias in aliases]

        section = str(page["section"])
        if section not in SECTION_INFO:
            raise ValueError(f"{page_id}: unknown manual section {section}")
        page["section_title"] = SECTION_INFO[section][1]

        packages_any = page.get("packages_any", [])
        if not isinstance(packages_any, list) or not all(
            isinstance(package, str) and package for package in packages_any
        ):
            raise ValueError(f"{page_id}: packages_any must contain package IDs")
        unknown = sorted(set(packages_any) - known_packages)
        if unknown:
            raise ValueError(f"{page_id}: unknown packages: {', '.join(unknown)}")
        page["packages_any"] = packages_any
        page["path"] = path
        page["markdown"] = markdown
        page["body"] = markdown_to_terminal_text(markdown)
        page["contract"] = extract_quick_reference(markdown, path)
        pages.append(page)

    if not pages:
        raise ValueError("manual source must contain at least one Markdown page")

    seen_names: dict[str, str] = {}
    for page in pages:
        page_id = str(page["id"])
        for name in [page_id, *page["aliases"]]:
            folded = str(name).casefold()
            owner = seen_names.get(folded)
            if owner is not None:
                raise ValueError(
                    f"manual topic name {name} is shared by {owner} and {page_id}"
                )
            seen_names[folded] = page_id
    return sorted(
        pages,
        key=lambda page: (
            SECTION_INFO[str(page["section"])][0],
            str(page["title"]).casefold(),
            str(page["id"]),
        ),
    )


def render_header(pages: list[dict[str, object]], source: Path) -> str:
    lines = [
        "/* Generated by scripts/generate_manual.py. Do not edit. */",
        f"/* Source: {source.name}/*.md */",
        "#pragma once",
        "",
        "static const solar_os_manual_page_t SOLAR_OS_MANUAL_GENERATED_PAGES[] = {",
    ]
    for page in pages:
        packages = list(page["packages_any"])
        if packages:
            lines.append(
                "#if " + " || ".join(package_macro(package) for package in packages)
            )
        aliases = "\n".join(page["aliases"])
        lines.extend(
            [
                "    {",
                f"        .id = {c_string(str(page['id']))},",
                f"        .title = {c_string(str(page['title']))},",
                f"        .section = {c_string(str(page['section']))},",
                f"        .section_title = {c_string(str(page['section_title']))},",
                f"        .summary = {c_string(str(page['summary']))},",
                f"        .aliases = {c_string(aliases)},",
                f"        .keywords = {c_string(str(page['keywords']))},",
                f"        .body = {c_string(str(page['body']))},",
                f"        .contract = {c_string(str(page['contract']))},",
                "#if SOLAR_OS_PACKAGE_APP_READER",
                f"        .markdown = {c_string(str(page['markdown']))},",
                "#else",
                "        .markdown = NULL,",
                "#endif",
                "    },",
            ]
        )
        if packages:
            lines.append("#endif")
    lines.extend(
        [
            "};",
            "",
            "#define SOLAR_OS_MANUAL_GENERATED_PAGE_COUNT \\",
            "    (sizeof(SOLAR_OS_MANUAL_GENERATED_PAGES) / \\",
            "     sizeof(SOLAR_OS_MANUAL_GENERATED_PAGES[0]))",
            "",
        ]
    )
    return "\n".join(lines)


def render_catalog(
    pages: list[dict[str, object]], source: Path, version: str
) -> str:
    catalog_pages: list[dict[str, object]] = []
    revision_hash = hashlib.sha256()
    for page in pages:
        path = Path(page["path"])
        markdown = path.read_bytes()
        digest = hashlib.sha256(markdown).hexdigest()
        relative = path.relative_to(source.parent).as_posix()
        revision_hash.update(relative.encode("utf-8"))
        revision_hash.update(b"\0")
        revision_hash.update(digest.encode("ascii"))
        revision_hash.update(b"\n")
        catalog_pages.append(
            {
                "id": page["id"],
                "title": page["title"],
                "section": page["section"],
                "section_title": page["section_title"],
                "summary": page["summary"],
                "aliases": page["aliases"],
                "keywords": page["keywords"],
                "packages_any": page["packages_any"],
                "path": relative,
                "size": len(markdown),
                "sha256": digest,
                "reference": page["contract"],
            }
        )
    catalog = {
        "schema": "solaros.manual_catalog",
        "schema_version": 1,
        "firmware_version": version,
        "revision": revision_hash.hexdigest()[:16],
        "pages": catalog_pages,
    }
    return json.dumps(catalog, indent=2, ensure_ascii=True) + "\n"


def render_github_index(pages: list[dict[str, object]]) -> str:
    lines = [
        "# SolarOS User Manual",
        "",
        "This is the canonical documentation used by GitHub, the generated "
        "solar-os.eu website, the signed on-device `docs` browser, `man`, "
        "and the native agent reference tool.",
        "",
    ]
    current_section = None
    for page in pages:
        section = str(page["section"])
        if section != current_section:
            if current_section is not None:
                lines.append("")
            lines.extend((f"## {page['section_title']}", ""))
            current_section = section
        path = Path(page["path"])
        lines.append(
            f"- [{page['title']}]({path.name}) — {page['summary']}"
        )
    lines.extend(
        (
            "",
            "The TOML frontmatter on each topic controls package availability, "
            "search metadata, and placement in the documentation tree. Edit "
            "the topic itself; do not maintain a separate device or website copy.",
            "",
        )
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--catalog-output", type=Path)
    parser.add_argument("--github-index-output", type=Path)
    parser.add_argument("--version")
    args = parser.parse_args()
    if (
        args.output is None
        and args.catalog_output is None
        and args.github_index_output is None
    ):
        parser.error(
            "at least one of --output, --catalog-output, or "
            "--github-index-output is required"
        )
    if args.catalog_output is not None and not args.version:
        parser.error("--version is required with --catalog-output")

    pages = load_pages(args.input, args.packages)
    if args.output is not None:
        output = render_header(pages, args.input)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_text() != output:
            args.output.write_text(output)
    if args.catalog_output is not None:
        catalog = render_catalog(pages, args.input, args.version)
        args.catalog_output.parent.mkdir(parents=True, exist_ok=True)
        if (
            not args.catalog_output.exists()
            or args.catalog_output.read_text() != catalog
        ):
            args.catalog_output.write_text(catalog)
    if args.github_index_output is not None:
        index = render_github_index(pages)
        args.github_index_output.parent.mkdir(parents=True, exist_ok=True)
        if (
            not args.github_index_output.exists()
            or args.github_index_output.read_text() != index
        ):
            args.github_index_output.write_text(index)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
