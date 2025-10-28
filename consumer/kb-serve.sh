#!/usr/bin/env bash

set -e

SOCKET=/tmp/kb.sock
PORT=1234

type-ascii --socket "$SOCKET" &
PID_TYPER=$!
trap "kill $PID_TYPER" EXIT

socat TCP-LISTEN:$PORT,reuseaddr,fork UNIX-CONNECT:"$SOCKET"
