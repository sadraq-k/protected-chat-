#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
proj_in_1file.py

پروژه‌ی کد رو (به همراه متن OCR‌شده‌ی عکس‌هایی مثل دیاگرام‌ها) توی یک فایل
Markdown سبک و کم‌حجم پیاده می‌کنه - مناسب برای دادن مستقیم به AI (توکن کمی
مصرف می‌کنه، برخلاف Word که حجم/توکن اضافه داره).

ترتیب خروجی دقیقاً مطابق ساختار پوشه‌هاست: اول درخت کامل پروژه، بعد به
ترتیب هر پوشه با فایل‌های کدش (داخل بلاک کد با syntax highlighting) و
عکس‌هاش (متن OCR شده‌شون).

پیش‌نیاز OCR (اختیاری - اگه نصب نباشه فقط هشدار می‌ده و از عکس‌ها رد می‌شه):
    pip install pytesseract Pillow --break-system-packages
    (+ خود موتور tesseract باید روی سیستم نصب باشه)

استفاده:
    python proj_in_1file.py                     -> از پوشه‌ی جاری شروع می‌کنه
    python proj_in_1file.py "مسیر پروژه"
    python proj_in_1file.py "مسیر پروژه" -o out.md
    python proj_in_1file.py --no-ocr             -> عکس‌ها رو کلاً رد کن
"""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

try:
    import pytesseract
    OCR_AVAILABLE = True
except ImportError:
    OCR_AVAILABLE = False

# ---------------------------------------------------------------------------
# تنظیمات
# ---------------------------------------------------------------------------

SELF_NAME = "proj_in_1file"

IGNORE_DIR_NAMES = {
    ".vs", "bin", "obj", "Debug", "Release", "x64", "x86",
    "ipch", "packages", "TestResults", ".vscode",
    ".git", ".svn", ".hg", "node_modules", "__pycache__",
    ".idea", ".pytest_cache", "dist", "build",
}

IGNORE_FILE_EXTENSIONS = {
    ".suo", ".user", ".sdf", ".opensdf", ".db", ".ipch",
    ".aps", ".ncb", ".pch", ".pdb", ".ilk", ".exp", ".lib",
    ".obj", ".o", ".idb", ".tlog", ".log", ".cache",
    ".vcxproj.filters", ".vcxproj.user", ".sln.docstates",
    ".dll", ".exe", ".manifest",
    ".pyc", ".pyo", ".class",
}

IGNORE_FILE_NAMES = {"thumbs.db", ".ds_store"}

# پسوند -> زبان برای syntax highlighting در بلاک کد مارک‌داون
CODE_LANG_MAP = {
    ".c": "c", ".h": "c", ".cpp": "cpp", ".hpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
    ".py": "python", ".java": "java", ".cs": "csharp", ".js": "javascript",
    ".ts": "typescript", ".jsx": "jsx", ".tsx": "tsx", ".html": "html",
    ".css": "css", ".sql": "sql", ".json": "json", ".xml": "xml",
    ".yaml": "yaml", ".yml": "yaml", ".sh": "bash", ".bat": "batch",
    ".ps1": "powershell", ".md": "markdown",
    ".vcxproj": "xml", ".sln": "text", ".csproj": "xml",
    ".props": "xml", ".targets": "xml",
}

IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".tiff", ".tif", ".gif", ".webp"}
OCR_LANGUAGES = "eng"


# ---------------------------------------------------------------------------
# فیلترها
# ---------------------------------------------------------------------------

def should_ignore_dir(dirname: str) -> bool:
    return dirname in IGNORE_DIR_NAMES or dirname.startswith(".")


def is_self_file(filename: str) -> bool:
    return Path(filename).stem.lower().startswith(SELF_NAME.lower())


def should_ignore_file(filename: str) -> bool:
    if is_self_file(filename):
        return True
    lower = filename.lower()
    if lower in IGNORE_FILE_NAMES:
        return True
    return any(lower.endswith(ext) for ext in IGNORE_FILE_EXTENSIONS)


def is_code_file(filename: str) -> bool:
    return Path(filename).suffix.lower() in CODE_LANG_MAP


def is_image_file(filename: str) -> bool:
    return Path(filename).suffix.lower() in IMAGE_EXTENSIONS


def ocr_extract_text(image_path: Path) -> str:
    if not OCR_AVAILABLE:
        return "*[OCR در دسترس نیست - pytesseract/Pillow نصب نشده]*"
    try:
        text = pytesseract.image_to_string(Image.open(image_path), lang=OCR_LANGUAGES).strip()
        return text if text else "*[متنی در عکس پیدا نشد]*"
    except Exception as e:
        return f"*[خطا در OCR: {e}]*"


# ---------------------------------------------------------------------------
# پیمایش پروژه
# ---------------------------------------------------------------------------

def build_tree_lines(root: Path) -> list:
    lines = [f"{root.name}/"]

    def walk(dir_path: Path, prefix: str):
        try:
            entries = sorted(dir_path.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))
        except PermissionError:
            return
        dirs = [e for e in entries if e.is_dir() and not should_ignore_dir(e.name)]
        files = [e for e in entries if e.is_file() and not should_ignore_file(e.name)]
        combined = dirs + files
        for i, entry in enumerate(combined):
            is_last = i == len(combined) - 1
            connector = "└── " if is_last else "├── "
            if entry.is_dir():
                lines.append(f"{prefix}{connector}{entry.name}/")
                walk(entry, prefix + ("    " if is_last else "│   "))
            else:
                lines.append(f"{prefix}{connector}{entry.name}")

    walk(root, "")
    return lines


def build_ordered_entries(root: Path) -> list:
    entries = []

    def walk(dir_path: Path, depth: int):
        try:
            items = sorted(dir_path.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))
        except PermissionError:
            return
        dirs = [e for e in items if e.is_dir() and not should_ignore_dir(e.name)]
        files = [e for e in items if e.is_file() and not should_ignore_file(e.name)]

        for f in files:
            rel = f.relative_to(root)
            if is_code_file(f.name):
                try:
                    content = f.read_text(encoding="utf-8", errors="replace")
                except Exception as e:
                    content = f"[خطا در خواندن فایل: {e}]"
                entries.append({"type": "code", "path": str(rel), "content": content})
            elif is_image_file(f.name):
                entries.append({"type": "image", "path": str(rel), "abs_path": f})

        for d in dirs:
            rel = d.relative_to(root)
            entries.append({"type": "heading", "path": str(rel), "depth": depth})
            walk(d, depth + 1)

    walk(root, 1)
    return entries


# ---------------------------------------------------------------------------
# ساخت خروجی Markdown
# ---------------------------------------------------------------------------

def fence_for(content: str) -> str:
    """اگه محتوا خودش ``` داشته باشه، فنس رو بلندتر بگیر که خراب نشه."""
    longest = 0
    run = 0
    for ch in content:
        if ch == "`":
            run += 1
            longest = max(longest, run)
        else:
            run = 0
    return "`" * max(3, longest + 1)


def build_markdown(root: Path, tree_lines: list, entries: list, do_ocr: bool) -> str:
    parts = []
    parts.append(f"# {root.name}\n")
    parts.append("## ساختار پروژه\n")
    parts.append("```\n" + "\n".join(tree_lines) + "\n```\n")

    heading_prefix = {1: "##", 2: "###", 3: "####", 4: "#####", 5: "######"}

    for entry in entries:
        if entry["type"] == "heading":
            prefix = heading_prefix.get(entry["depth"], "######")
            parts.append(f"{prefix} {entry['path']}/\n")

        elif entry["type"] == "code":
            lang = CODE_LANG_MAP.get(Path(entry["path"]).suffix.lower(), "")
            fence = fence_for(entry["content"])
            parts.append(f"**`{entry['path']}`**\n")
            parts.append(f"{fence}{lang}\n{entry['content']}\n{fence}\n")

        elif entry["type"] == "image":
            parts.append(f"**`{entry['path']}`** *(عکس - متن استخراج‌شده با OCR)*\n")
            if do_ocr:
                text = ocr_extract_text(entry["abs_path"])
            else:
                text = "*[OCR غیرفعال بود]*"
            parts.append(f"> {text}\n")

    return "\n".join(parts)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="جمع‌آوری کد (و متن OCR عکس‌ها) یک پروژه در یک فایل Markdown سبک."
    )
    parser.add_argument(
        "project_path", nargs="?", default=".",
        help="مسیر پوشه‌ی پروژه (پیش‌فرض: پوشه‌ی جاری)"
    )
    parser.add_argument(
        "-o", "--output", default=f"{SELF_NAME}.md",
        help=f"نام فایل خروجی (پیش‌فرض: {SELF_NAME}.md)"
    )
    parser.add_argument(
        "--no-ocr", action="store_true",
        help="عکس‌ها را کلاً نادیده بگیر"
    )
    args = parser.parse_args()

    root = Path(args.project_path).resolve()
    if not root.is_dir():
        print(f"خطا: مسیر '{root}' یک پوشه معتبر نیست.", file=sys.stderr)
        sys.exit(1)

    output_path = Path(args.output).resolve()
    if output_path.suffix.lower() != ".md":
        output_path = output_path.with_suffix(".md")

    tree_lines = build_tree_lines(root)
    entries = build_ordered_entries(root)

    n_code = sum(1 for e in entries if e["type"] == "code")
    n_img = sum(1 for e in entries if e["type"] == "image")

    do_ocr = not args.no_ocr and n_img > 0
    if do_ocr and not OCR_AVAILABLE:
        print(
            "هشدار: pytesseract/Pillow نصب نیست، عکس‌ها بدون OCR رد می‌شوند.\n"
            "برای فعال‌سازی: pip install pytesseract Pillow --break-system-packages",
            file=sys.stderr,
        )

    md = build_markdown(root, tree_lines, entries, do_ocr)
    output_path.write_text(md, encoding="utf-8")

    print(f"فایل Markdown ساخته شد: {output_path}  ({n_code} فایل کد، {n_img} عکس)")


if __name__ == "__main__":
    main()
