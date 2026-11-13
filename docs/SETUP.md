# Setup (Windows laptop → WSL2)

The code lives in WSL2's Linux filesystem and on GitHub. Nothing builds from `C:\` or OneDrive.

## 1. Install WSL2 + Ubuntu (once)

In PowerShell **as Administrator**:

```powershell
wsl --install -d Ubuntu
```

Restart, open "Ubuntu" from the Start menu, and create a username and password.

## 2. Install tools (once, in the Ubuntu terminal)

```sh
sudo apt update
sudo apt install -y git build-essential make qemu-system-x86 gh \
  unzip rsync bc cpio file wget python3 libncurses-dev
```

`libncurses-dev` is only needed for `make menuconfig`.

Install Claude Code inside Ubuntu as well, so it runs where the code builds:

```sh
curl -fsSL https://claude.ai/install.sh | bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

## 3. Get the repo (once)

```sh
gh auth login
gh repo clone maxcmperkins/PerkinsOS ~/PerkinsOS
```

## 4. Let QEMU use KVM (once, optional)

Without this, `make run` prints a warning and uses software emulation. That still boots in about 10 seconds; KVM makes it faster.

```sh
sudo usermod -aG kvm $USER
printf '[boot]\ncommand = "chown root:kvm /dev/kvm && chmod 660 /dev/kvm"\n' | sudo tee -a /etc/wsl.conf
```

Then, from PowerShell, `wsl --shutdown`, and reopen Ubuntu. Check with `ls -l /dev/kvm`: the group should be `kvm`. A udev rule alone did not change the device's permissions on this WSL setup, which is why this uses `wsl.conf`. (Not yet tested on this machine.)

## 5. Every session

```sh
cd ~/PerkinsOS
make image    # first build ~25 min; later builds only redo what changed
make run      # boots PerkinsOS; the QEMU window appears on your Windows desktop
```

- Quit QEMU: close the window, or press Ctrl-A then X in the terminal.
- The terminal you ran `make run` in is the VM's serial console: log in as `root` (no password).
- After editing `compositor/` or `shell/`: `make compositor` or `make shell` (seconds), then `make run`.
- `make test` and `make test-desktop` boot headless and check the result.

From Windows Explorer, the repo is at `\\wsl$\Ubuntu\home\<you>\PerkinsOS`.
