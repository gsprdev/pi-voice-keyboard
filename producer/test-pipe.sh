#!/bin/bash

set -e

localHort="127.0.0.1"
localPort=1234
remoteHost="pi02w.local"
remotePort=1234

# Tunnel
ssh -N -L $localPort:$localHort:$remotePort $remoteHost &
SSH_PID=$!
SSH_EXIT=$?
if [ $SSH_EXIT != 0 ]; then
    echo "SSH FAILED! code $SSH_EXIT"
fi
trap "kill $SSH_PID" EXIT
until nc -z 127.0.0.1 $localPort; do sleep 0.1; done
exec 3<>/dev/tcp/$localHort/$localPort

# Terminal setup
stty -echo -icanon time 0 min 0
trap "stty sane; echo; exit" INT TERM

buffer=""
flush_interval=0.1  # 100ms

while true; do
  # Use IFS= read to preserve whitespace
  if IFS= read -r -n1 -t "$flush_interval" char; then
    buffer+="$char"
    # Flush on space, newline, or punctuation
    if [[ "$char" =~ [[:space:]] || "$char" =~ [.!?] ]]; then
      printf "%s" "$buffer" >&3
      buffer=""
    fi
  else
    # Timeout occurred, flush buffer if non-empty
    if [[ -n "$buffer" ]]; then
      printf "%s" "$buffer" >&3
      buffer=""
    fi
  fi
done
