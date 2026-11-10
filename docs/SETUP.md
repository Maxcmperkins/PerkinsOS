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
  unzip rsync bc cpio file wget python3
```

Install Claude Code inside Ubuntu as well, so it runs where the code builds.

## 3. Move the starter files into WSL and push to GitHub (once)

```sh
cp -r "/mnt/c/Users/Max/OneDrive/Desktop/Digi art/PerkinsOS" ~/PerkinsOS
cd ~/PerkinsOS
git init -b main
git add .
git commit -m "Initial commit: plan, README, CLAUDE.md"
gh auth login
gh repo create PerkinsOS --public --source=. --push
```

After this, delete the OneDrive copy. `~/PerkinsOS` plus GitHub are the only copies.

## 4. Every session

```sh
cd ~/PerkinsOS
code .        # VS Code, opened inside WSL (needs the "WSL" extension)
claude        # Claude Code, in the same folder
make run      # boots PerkinsOS; the QEMU window appears on your Windows desktop
git add -p && git commit && git push
```

From Windows Explorer, the repo is at `\\wsl$\Ubuntu\home\<you>\PerkinsOS`.
