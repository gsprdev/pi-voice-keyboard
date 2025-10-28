#!/bin/bash

set -e

localHost="127.0.0.1"
localPort=1234
remoteHost="pi02w.local"
remotePort=1234

flatpak run net.mkiol.SpeechNote --action start-listening-active-window &
trap "sleep 1; flatpak run net.mkiol.SpeechNote --action cancel" EXIT

# Tunnel
ssh -N -L $localPort:$localHost:$remotePort $remoteHost &
SSH_PID=$!
trap "kill $SSH_PID" EXIT
until nc -z $localHost $localPort; do sleep 0.1; done
exec 3<>/dev/tcp/$localHost/$localPort

# Terminal setup
stty -echo -icanon time 0 min 0
trap "stty sane; echo; exit" INT TERM

buffer=""
flush_interval=0.1  # 100ms
#flush_interval=0.5  # 500ms

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
