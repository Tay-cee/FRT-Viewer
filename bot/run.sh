#!/bin/sh
cd "$(dirname "$0")" || exit 1
if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt || exit 1
fi
[ -x ../frt ] || make -C .. || exit 1
exec .venv/bin/python bot.py
