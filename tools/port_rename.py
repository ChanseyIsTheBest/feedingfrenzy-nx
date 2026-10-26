"""One-off (2026-10-26): turns the copy of bejeweled3-nx's app and tools into feedingfrenzy-nx's names, in place.

Only the project's identifiers and file names change: bejeweled3 -> feedingfrenzy, Bejeweled3 -> FeedingFrenzy,
BEJEWELED3 -> FEEDINGFRENZY (keys, macros, targets, files). Prose keeps "Bejeweled 3", "bejeweled3-nx" and "Peggle": the comments
record what was found in those ports, and stay true as history. Paths that must name this project (the NRO's place on
the SD card, the delivery folder) are changed by hand where they are used.

  python -I tools/port_rename.py app tools

(bejeweled3-nx's version turned peggle1-nx's copy into its names; peggle1-nx's turned peggle2-nx's.)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT_EXT = {'.cpp', '.h', '.hpp', '.c', '.inc', '.in', '.txt', '.cmake', '.json', '.toml', '.py', '.ps1', '.sh', '.md',
            '.hlsl', '.glsl', '.frag', '.vert', '.comp', '.mjs', '.js', '.yml', '.bat', ''}
SKIP_DIRS = {'thirdparty', 'generated', '__pycache__'}
IDENT = [
    (re.compile(r'bejeweled3(?!-nx)'), 'feedingfrenzy'),
    (re.compile(r'Bejeweled3'), 'FeedingFrenzy'),
    (re.compile(r'BEJEWELED3'), 'FEEDINGFRENZY'),
]
SELF = os.path.abspath(__file__)


def rename_text(text):
    for rx, rep in IDENT:
        text = rx.sub(rep, text)
    return text


def main():
    changed = renamed = 0
    for folder in sys.argv[1:]:
        for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, folder), topdown=False):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            if any(part in SKIP_DIRS for part in os.path.relpath(dirpath, ROOT).split(os.sep)):
                continue
            for name in filenames:
                path = os.path.join(dirpath, name)
                if os.path.abspath(path) == SELF:
                    continue
                ext = os.path.splitext(name)[1].lower()
                if ext in TEXT_EXT:
                    raw = open(path, 'rb').read()
                    if b'\0' not in raw[:4096]:
                        try:
                            text = raw.decode('utf-8')
                        except UnicodeDecodeError:
                            text = None
                        if text is not None:
                            out = rename_text(text)
                            if out != text:
                                open(path, 'wb').write(out.encode('utf-8'))
                                changed += 1
                new_name = rename_text(name)
                if new_name != name:
                    os.replace(path, os.path.join(dirpath, new_name))
                    renamed += 1
    print(f'{changed} files changed, {renamed} files renamed')


if __name__ == '__main__':
    main()
