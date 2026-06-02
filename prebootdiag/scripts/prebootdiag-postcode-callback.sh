#!/bin/sh
# Notify prebootdiag that a specific pre-boot post code has been observed.
# Invoked by the systemd template unit
#   com.nvidia.PreBootDiag.PostCodeCallback@<PostCodeName>.service
# instantiated by phosphor-post-code-manager via post-code-handlers.json.
# The post code name is passed as $1 (from the unit's %i instance specifier).
# We send com.nvidia.PreBootDiag.App.Notify with the generic PostCodeReceived
# state and a JSON payload carrying the PostCodeName.
set -eu

if [ $# -ne 1 ]; then
    cat >&2 <<EOF
Usage: $0 <PostCodeName>

  PostCodeName: one of
    PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG
    MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY
EOF
    exit 1
fi

NAME="$1"

# Defense in depth: catch a typo in post-code-handlers.json before reaching
# busctl. prebootdiag also rejects unknowns, but failing here ties the error
# to the systemd unit's journal entry.
case "$NAME" in
    PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG|MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY)
        ;;
    *)
        echo "Unknown PostCodeName '$NAME'" >&2
        exit 1
        ;;
esac

STATE="com.nvidia.PreBootDiag.App.StateType.PostCodeReceived"
PAYLOAD="{\"PostCodeName\":\"${NAME}\"}"

exec busctl call \
    com.nvidia.PreBootDiag \
    /com/nvidia/prebootdiag \
    com.nvidia.PreBootDiag.App \
    Notify ss "$STATE" "$PAYLOAD"
