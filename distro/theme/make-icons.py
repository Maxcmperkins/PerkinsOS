#!/usr/bin/env python3
"""Draw the PerkinsOS file icons: green outlines, like the HUD.

Usage: make-icons.py OUTPUT_DIR

One SVG per icon, 48x48, which programs scale to whatever size they need.
Only the icons a file manager shows most are here; everything else comes
from Adwaita (see index.theme).
"""
import os
import sys

FOLDER = '<path d="M5 12h14l4 5h20v23H5z"/>'
PAGE = '<path d="M11 4h18l9 9v31H11z"/><path d="M29 4v9h9"/>'
DISK = '<rect x="5" y="14" width="38" height="20"/><path d="M5 27h38M34 31h4"/>'
BIN = '<path d="M11 14l3 30h20l3-30M7 14h34M18 14V7h12v7M19 21v16M24 21v16M29 21v16"/>'

# folder/name: (drawing, other names for it)
ICONS = {
    "places/folder": (FOLDER, ["inode-directory", "folder-open"]),
    "places/user-home": (FOLDER + '<path d="M16 33v-6l8-6 8 6v6z"/>', ["folder-home"]),
    "places/user-desktop": (FOLDER + '<rect x="15" y="23" width="18" height="11"/>', []),
    "places/folder-documents": (FOLDER + '<path d="M15 24h18M15 29h18M15 34h11"/>', []),
    "places/folder-download": (FOLDER + '<path d="M24 22v11M18 28l6 6 6-6"/>', ["folder-downloads"]),
    "places/folder-pictures": (FOLDER + '<path d="M14 35l7-9 5 6 3-3 5 6z"/>', []),
    "places/folder-music": (FOLDER + '<path d="M21 34V23l9-2v11"/><circle cx="18" cy="34" r="3"/><circle cx="27" cy="32" r="3"/>', []),
    "places/folder-videos": (FOLDER + '<path d="M20 23v12l10-6z"/>', []),
    "places/folder-templates": (FOLDER, []),
    "places/folder-publicshare": (FOLDER, []),
    "places/user-trash": (BIN, ["user-trash-full"]),
    "places/network-workgroup": ('<circle cx="24" cy="24" r="19"/><path d="M5 24h38M24 5c-12 10-12 28 0 38M24 5c12 10 12 28 0 38"/>', ["network-server"]),
    "devices/drive-harddisk": (DISK, ["drive-harddisk-system"]),
    "devices/drive-removable-media": (DISK + '<path d="M18 14V6h12v8"/>', ["drive-harddisk-usb", "media-removable"]),
    "devices/computer": ('<rect x="5" y="8" width="38" height="25"/><path d="M24 33v8M14 41h20"/>', []),
    "mimetypes/text-x-generic": (PAGE + '<path d="M17 21h15M17 27h15M17 33h10"/>', ["text-plain", "application-x-zerosize", "unknown"]),
    "mimetypes/text-x-script": (PAGE + '<path d="M17 22l6 5-6 5M25 33h7"/>', ["application-x-executable"]),
    "mimetypes/text-html": (PAGE + '<path d="M20 22l-5 6 5 6M28 22l5 6-5 6"/>', []),
    "mimetypes/image-x-generic": (PAGE + '<path d="M15 38l7-11 5 7 3-4 4 8z"/>', []),
    "mimetypes/audio-x-generic": (PAGE + '<path d="M22 36V23l8-2v12"/><circle cx="19" cy="36" r="3"/><circle cx="27" cy="33" r="3"/>', []),
    "mimetypes/video-x-generic": (PAGE + '<path d="M20 22v14l11-7z"/>', []),
    "mimetypes/application-pdf": (PAGE + '<path d="M17 36V23h6a4 4 0 0 1 0 8h-6"/>', ["x-office-document"]),
    "mimetypes/package-x-generic": ('<path d="M6 14l18-9 18 9v21l-18 9-18-9zM6 14l18 9 18-9M24 23v21"/>', []),
}

SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" '
       'viewBox="0 0 48 48"><g fill="#04170a" stroke="#33ff66" '
       'stroke-width="2.4" stroke-linejoin="round" stroke-linecap="round">'
       '%s</g></svg>\n')


def main():
    out = sys.argv[1]
    for path, (drawing, others) in ICONS.items():
        folder, name = path.split("/")
        os.makedirs(os.path.join(out, "scalable", folder), exist_ok=True)
        for n in [name] + others:
            with open(os.path.join(out, "scalable", folder, n + ".svg"), "w") as f:
                f.write(SVG % drawing)


if __name__ == "__main__":
    main()
