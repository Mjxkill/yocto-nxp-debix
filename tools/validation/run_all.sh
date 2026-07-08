#!/bin/bash
# Campagne de validation A.L.A. — rejoue P2→P5 sur la carte.
# Usage : ./run_all.sh [ip]     (défaut 192.168.0.198)
# Les scripts s'exécutent SUR la carte (scp puis python3 local au board).
# P6 (reboot) reste manuel : ssh root@IP reboot, puis re-vérifier
# 'fx-restore: alsactl vérifié' dans journalctl -u ala-fx-restore.
set -e
IP=${1:-192.168.0.198}
DIR=$(dirname "$0")

echo "=== Backup préalable"
ssh root@$IP 'B=/root/tests/backup_$(date +%Y%m%d_%H%M); mkdir -p $B;
  cp -a /var/lib/mixer-pro /var/lib/alsa/asound.state /var/lib/ala $B/ 2>/dev/null; echo $B'

for p in p2 p3 p4 p5; do
    echo "=== validate_$p"
    scp -O -q "$DIR/validate_$p.py" root@$IP:/root/tests/
    ssh root@$IP "python3 /root/tests/validate_$p.py"
done

echo "=== Journaux (prio >= warning depuis boot)"
ssh root@$IP 'for s in mixer-pro mixer-gui-http mixer-console midi-expander anti-larsen; do
  n=$(journalctl -u $s -b -p warning --no-pager 2>/dev/null | grep -vc "^--"); echo "$s: $n"; done'
echo "=== FIN — voir docs/TESTS/RAPPORT_VALIDATION_V13.1_2026-07-08.md pour la référence"
