#!/usr/bin/env bash
# Right-click menus: Py2C Studio, C-OS Studio and the File Manager (each started from the Terminal; see tools/ctxmenu_*_peer.py). Messenger: tools/run_ctxmenu_msg_test.sh.
D="$(dirname "$0")"; RC=0
for p in py2c studio files; do
  echo "== $p"; env PEER=tools/ctxmenu_${p}_peer.py bash "$D/run_terminal_test.sh" | grep -E "^(PASS|FAIL)" ; [ "${PIPESTATUS[0]}" -eq 0 ] || RC=1
done
exit $RC
