# compositor/

The PerkinsOS Wayland compositor: stock TinyWL from wlroots 0.19.2 plus the changes marked `PerkinsOS:` in [tinywl.c](tinywl.c).

- Commit `068d7ce` is the untouched fork. `git diff 068d7ce -- compositor/` shows everything added since.
- Build: `make compositor` from the top of the repo (the rule is in `buildroot/package/perkinsos-compositor/`).
- If the screen stays black after a change, log in on the serial console and read `/var/log/desktop.log`.

What was added, in the order it was built:

| Change | Where to look |
| --- | --- |
| Wallpaper | `server_new_output`, `wallpaper_tree` |
| Window borders, coloured by focus | `toplevel_update_borders`, `focus_toplevel` |
| Keybindings (Alt+Enter, Alt+Q, Alt+P) | `handle_keybinding`, `spawn` |
| Layer-shell | `server_new_layer_surface`, `arrange_layers`, `layer_surface_*` |
| Maximize into the area the dock leaves free | `xdg_toplevel_request_maximize`, `usable_area` |

[docs/WEEK3.md](../docs/WEEK3.md) and [docs/WEEK4.md](../docs/WEEK4.md) explain the ideas behind each.
