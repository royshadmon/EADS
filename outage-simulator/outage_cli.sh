#!/bin/bash
# =============================================================================
# outage_cli.sh  —  Terminal control helper for the EADS Outage Proxy
#
# Usage:
#   ./outage_cli.sh <operator> <command>
#
# Operators:
#   1   →  proxy on port 9001  (→ AnyLog operator 1, :32149)
#   2   →  proxy on port 9002  (→ AnyLog operator 2, :32249)
#   3   →  proxy on port 9003  (→ AnyLog operator 3, :32349)
#   all →  applies command to all three operators
#
# Commands:
#   start   — trigger spike → outage on the chosen operator
#   stop    — end outage, resume normal forwarding
#   status  — show current proxy state
#   health  — liveness check
#
# Examples:
#   ./outage_cli.sh 1 start          # start outage on operator 1
#   ./outage_cli.sh 1 stop           # end outage on operator 1
#   ./outage_cli.sh all status       # status of all three proxies
# =============================================================================

set -euo pipefail

# ── Port lookup (case statement — compatible with macOS bash 3.x) ─────────────
get_port() {
    case "$1" in
        1) echo "9001" ;;
        2) echo "9002" ;;
        3) echo "9003" ;;
        *) echo "" ;;
    esac
}

# ── Helpers ───────────────────────────────────────────────────────────────────
usage() {
    echo "Usage: ./outage_cli.sh <operator> <command>"
    echo ""
    echo "Operators: 1, 2, 3, all"
    echo "Commands:  start, stop, status, health"
    exit 1
}

pretty() {
    if command -v jq &>/dev/null; then
        echo "$1" | jq .
    else
        echo "$1"
    fi
}

run_command() {
    local port="$1"
    local cmd="$2"
    local base="http://127.0.0.1:${port}"
    local resp

    echo "────────────────────────────────────────"
    echo "  Proxy port : $port"
    echo "  Command    : $cmd"
    echo "────────────────────────────────────────"

    case "$cmd" in
        start)
            resp=$(curl -s -X POST "${base}/sim-power-outage")
            pretty "$resp"
            ;;
        stop)
            resp=$(curl -s -X POST "${base}/end-power-outage")
            pretty "$resp"
            ;;
        status)
            resp=$(curl -s "${base}/status")
            pretty "$resp"
            ;;
        health)
            resp=$(curl -s "${base}/health")
            pretty "$resp"
            ;;
        *)
            echo "Unknown command: $cmd"
            usage
            ;;
    esac
    echo
}

# ── Argument parsing ──────────────────────────────────────────────────────────
if [[ $# -lt 2 ]]; then
    usage
fi

OPERATOR="$1"
COMMAND="$2"

if [[ "$OPERATOR" == "all" ]]; then
    for op in 1 2 3; do
        run_command "$(get_port $op)" "$COMMAND"
    done
else
    PORT=$(get_port "$OPERATOR")
    if [[ -z "$PORT" ]]; then
        echo "ERROR: unknown operator '$OPERATOR'. Must be 1, 2, 3, or 'all'."
        exit 1
    fi
    run_command "$PORT" "$COMMAND"
fi