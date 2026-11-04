#!/usr/bin/env bash
# Optional development helper: bake defaults into the gitignored main/secrets.h.
# Normal use does not need this file; the Passport starts a setup AP and collects
# WiFi, MA address/token and the bridge address on first boot.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$repo_root/main/secrets.h"
token_file="${MA_TOKEN_FILE:-}"

if [[ -f "$out" && "${1:-}" != "--force" ]]; then
  echo "Refusing to overwrite $out (pass --force to regenerate)." >&2
  exit 1
fi
if [[ -z "$token_file" ]]; then
  read -r -p "Path to the file holding the raw MA token: " token_file
fi
if [[ ! -f "$token_file" ]]; then
  echo "MA token file not found: $token_file" >&2
  exit 1
fi

read -r -p "WiFi SSID (optional dev default): " ssid
read -r -s -p "WiFi password (optional dev default): " pass
echo
read -r -p "MA host (optional dev default): " ma_host
read -r -p "MA port [8095]: " ma_port
ma_port="${ma_port:-8095}"
read -r -p "Bridge host (optional dev default): " bridge_host
read -r -p "Bridge port [8096]: " bridge_port
bridge_port="${bridge_port:-8096}"
read -r -p "Player id (optional; empty = auto-resolve): " player

token="$(tr -d '\r\n' < "$token_file")"
if [[ ! "$token" =~ ^[A-Za-z0-9._~+/=-]+$ ]]; then
  echo "Token file contains unexpected characters; aborting." >&2
  exit 1
fi

umask 077
cat > "$out" <<EOF
// Local-only development defaults. Gitignored; never commit.
// Normal use uses the Passport-XXXX setup AP instead.
#pragma once

#define APP_SECRET_WIFI_SSID "$ssid"
#define APP_SECRET_WIFI_PASS "$pass"
#define APP_SECRET_MA_HOST   "$ma_host"
#define APP_SECRET_MA_PORT   $ma_port
#define APP_SECRET_MA_TOKEN  "$token"
#define APP_SECRET_BRIDGE_HOST "$bridge_host"
#define APP_SECRET_BRIDGE_PORT $bridge_port
#define APP_SECRET_PLAYER_ID "$player"
EOF
chmod 600 "$out"
echo "Wrote $out"
