# shell/

The PerkinsOS dock, launcher and apps. **Max writes the code in this folder.**

Each folder here becomes one program in the image:

| Folder | Program | Started by |
| --- | --- | --- |
| `shell/dock/` | `/usr/bin/perkinsos-dock` | `perkinsos-session` at boot |
| `shell/launcher/` | `/usr/bin/perkinsos-launcher` | `perkinsos-session` at boot |
| `shell/NAME/` | `/usr/bin/perkinsos-NAME` | you, or the launcher |

All `.c` files in a folder are compiled together. There is no Makefile here: the build rule lives in [buildroot/package/perkinsos-shell/perkinsos-shell.mk](../buildroot/package/perkinsos-shell/perkinsos-shell.mk). See [docs/WEEK5.md](../docs/WEEK5.md).
