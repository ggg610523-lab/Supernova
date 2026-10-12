#!/bin/bash
clear
echo "=== win11wm Live System ==="
echo
echo "1. Start win11wm session (default)"
echo "2. Install using archinstall (TUI)"
echo "3. Open terminal"
echo
read -p "Choice [1-3]: " choice
case $choice in
  1) exec /usr/local/bin/win11-session;;
  2) if command -v archinstall >/dev/null; then exec archinstall; else echo "archinstall not found"; read; exec /usr/local/bin/win11-session; fi;;
  3) exec /usr/local/bin/win11-terminal;;
  *) exec /usr/local/bin/win11-session;;
esac
