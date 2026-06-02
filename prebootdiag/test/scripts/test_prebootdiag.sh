#!/bin/bash
# PreBootDiag end-to-end test script — push-based State interface
# Run on target BMC as root
#
# Architecture note (updated):
#   NSM events (CPU → nsmd) still flow via nsmd mock responder. nsmd now
#   pushes them into prebootdiag via com.nvidia.PreBootDiag.App.Notify
#   instead of the previous DiagSession property. prebootdiag itself is
#   driven purely by App.Notify calls — the two ordered boot-mode post
#   codes (PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG, then
#   MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY)
#   come from the postcode-manager and are simulated in this script via
#   busctl App.Notify calls carrying a JSON payload with PostCodeName.
#   Enable/disable is via the standard
#   xyz.openbmc_project.Object.Enable.Enabled property — there is no more
#   StartDiagBoot/AbortDiagBoot method.
#
# Coverage (maps to SWE-PLC-L1-SADD Section 10.1 test cases):
#   Precondition guards: TC-07 (InProgress rejection)
#   Happy-path lifecycle: TC-06, TC-15, TC-16, TC-20, TC-22, TC-27
#   NSM state variations: Multiple heartbeats (FR-09), multiple TIDs (Flow 3)
#   Failure & recovery:   Session reuse after abort (Section 7.3), error log (NFR-06),
#                          missing TID config abort (GAP 10), Disable mid-session (GAP 7)
#   Config readback:      Verify DiagSystemConfig/DiagConfig round-trip after seeding
#   Per-TID filtering:    Multi-TID config with per-TID requests (testplan.py Test 2)
#   Error result:         Firmware returns non-zero result code (testplan.py Test 1)
#   Concurrency:          TC-29 (duplicate start rejection)
#   Stress tests:         Rapid-fire signals, multi-TID rapid-fire, heartbeat bursts
#   Timeout enforcement:  PSC-FMC post-code timeout (30s, TC-18),
#                          event-idle timeout (3min, FR-09 / 7.4)
#
# Note: TC-05 (missing DiagConfig) is only testable via unit test — hasDiagConfig()
# checks property existence, not content, and the Settings property always exists.
# Unknown NSM state handling is also unit-test only (mock responder cannot inject
# arbitrary states). The MB2 post-code timeout (15min) is excluded too — its real
# wait duration is impractical for bench runs; the unit test fixture exercises
# the same path with a shortened timeout via setPostCodeGateWaitTimeoutForTest().

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PREBOOTDIAG_SVC="com.nvidia.PreBootDiag"
PREBOOTDIAG_OBJ="/com/nvidia/prebootdiag"
ENABLE_IFACE="xyz.openbmc_project.Object.Enable"
APP_IFACE="com.nvidia.PreBootDiag.App"

SETTINGS_SVC="xyz.openbmc_project.Settings"
DIAG_OBJ="/xyz/openbmc_project/Control/Diag"
DIAG_IFACE="xyz.openbmc_project.Control.Diag"

NSM_SVC="xyz.openbmc_project.NSM"

NSM_MOCK_SVC="xyz.openbmc_project.NSM.eid_14"
NSM_MOCK_OBJ="/xyz/openbmc_project/NSM/14"
NSM_MOCK_IFACE="xyz.openbmc_project.NSM.Device"

JOURNAL_UNIT="com.nvidia.PreBootDiag"

# StateType enum fully-qualified values (from App.interface.yaml)
STATE_PREFIX="com.nvidia.PreBootDiag.App.StateType"
S_POSTCODE="${STATE_PREFIX}.PostCodeReceived"
S_SYSCFG="${STATE_PREFIX}.SystemConfigRequested"

# PostCodeName values carried in the PostCodeReceived payload (ordered).
PC_PSC_FMC="PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG"
PC_MB2="MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY"

DIAG_CONFIG='[{"Tid":1,"TestDuration":2,"Loops":100,"LogLevel":1,"DynamicData":[170,187,204,221]}]'
DIAG_SYS_CONFIG='{"ConfigType":0,"TestDuration":1,"DynamicData":[3,1,2,4]}'

# Config Set 2: Multi-TID per-TID duration (testplan.py Test 2 — "ALL TIDs Duration Level 1")
DIAG_SYS_CONFIG_2='{"ConfigType":1,"TestDuration":0,"DynamicData":[3,1,2,4]}'
DIAG_CONFIG_2='[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicData":[0,0,0,0]},{"Tid":2,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicData":[0,0,0,0,0]},{"Tid":4,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicData":[0,0,0,0]}]'

# Invalid SystemConfig used to exercise nsmd's TestDuration validation
# (valid range is [0, 3]). Drives the "NSM push failure → session aborts"
# path added by the fix #5 review item.
DIAG_SYS_CONFIG_ERR='{"ConfigType":1,"TestDuration":4,"DynamicData":[0]}'

PAUSE=${PAUSE:-2}
STEP=0
FAILURES=0

# ─────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────

function step() {
    STEP=$((STEP + 1))
    echo ""
    echo -e "${CYAN}=== Step ${STEP}: $1 ===${NC}"
}

function info() {
    echo -e "${YELLOW}  -> $1${NC}"
}

function pass() {
    echo -e "${GREEN}  [PASS] $1${NC}"
}

function fail() {
    echo -e "${RED}  [FAIL] $1${NC}"
    FAILURES=$((FAILURES + 1))
}

function check_status() {
    local expected=$1
    local status
    status=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagStatus 2>&1) || true
    echo "  DiagStatus: $status"
    if echo "$status" | grep -q "y $expected"; then
        pass "DiagStatus is $expected as expected"
    else
        fail "DiagStatus expected $expected, got: $status"
    fi
}

# Poll DiagStatus until it matches the expected value or `timeout` seconds
# elapse. Useful when an event has been triggered but the service may still
# be awaiting an upstream Async.Set completion before it dequeues the next
# event — single-shot check_status can race the state transition.
function wait_for_status() {
    local expected=$1
    local timeout=${2:-10}
    local status
    local elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        status=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagStatus 2>&1) || true
        if echo "$status" | grep -q "y $expected"; then
            echo "  DiagStatus: $status (after ${elapsed}s)"
            pass "DiagStatus is $expected as expected"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "  DiagStatus: $status (timed out after ${timeout}s)"
    fail "DiagStatus expected $expected within ${timeout}s, got: $status"
}

function mark_journal() {
    # Save current journal line count as a checkpoint.
    # check_journal_for will only search lines added after this mark.
    JOURNAL_MARK=$(journalctl -u "$JOURNAL_UNIT" --no-pager 2>/dev/null | wc -l)
}

function check_journal_for() {
    local pattern=$1
    local label=${2:-"Journal contains '$pattern'"}
    local mark=${JOURNAL_MARK:-0}
    # Only check journal lines added since the last mark_journal call
    if journalctl -u "$JOURNAL_UNIT" --no-pager 2>/dev/null |
    tail -n +"$((mark + 1))" | grep -q "$pattern"; then
        pass "$label"
    else
        fail "$label"
    fi
}

function check_property_contains() {
    local prop=$1
    local expected=$2
    local label=${3:-"$prop contains '$expected'"}
    local actual
    actual=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" "$prop" 2>&1) || true
    echo "  $prop: $actual"
    if echo "$actual" | grep -q "$expected"; then
        pass "$label"
    else
        fail "$label"
    fi
}

function wait_settle() {
    sleep "$PAUSE"
}

# ─────────────────────────────────────────────
# bmcweb / Redfish HTTP layer helpers
# ─────────────────────────────────────────────
BMCWEB_BASE="${BMCWEB_BASE:-http://localhost}"
SYS_ID="${SYS_ID:-HGX_Baseboard_0}"
SYS_URL="$BMCWEB_BASE/redfish/v1/Systems/$SYS_ID"
PROC_URL="$SYS_URL/Oem/Nvidia"
BMCWEB_BODY="/tmp/test_prebootdiag_bmcweb_body.$$"
BMCWEB_BODY_FLAT="/tmp/test_prebootdiag_bmcweb_body_flat.$$"
trap 'rm -f "$BMCWEB_BODY" "$BMCWEB_BODY_FLAT"' EXIT

# Flatten pretty-printed JSON to a single line. busybox tr's interpretation of
# '\n\t' is unreliable across builds, so we use awk: read each line, print it
# with a trailing space, no newline. Result: one long line. Then squeeze
# repeated spaces.
function flatten_body() {
    awk '{printf "%s ", $0}' "$BMCWEB_BODY" 2>/dev/null \
        | tr -s ' ' >"$BMCWEB_BODY_FLAT" 2>/dev/null || true
}

# bmcweb_curl METHOD URL [DATA] [HEADER] -> echoes status code; body in $BMCWEB_BODY
function bmcweb_curl() {
    local m=$1 u=$2 d=${3-} h=${4:-Content-Type: application/json}
    if [ -n "$d" ]; then
        curl -s -o "$BMCWEB_BODY" -w '%{http_code}' -X "$m" -H "$h" -d "$d" "$u"
    else
        curl -s -o "$BMCWEB_BODY" -w '%{http_code}' -X "$m" "$u"
    fi
}

# bmcweb_assert_status ID "expected[ expected ...]" METHOD URL [DATA] [HDR]
function bmcweb_assert_status() {
    local id=$1 want=$2
    shift 2
    local got
    got=$(bmcweb_curl "$@")
    case " $want " in
        *" $got "*) pass "$id (HTTP $got)" ;;
        *) fail "$id: expected [$want], got $got — body: $(head -c 200 "$BMCWEB_BODY")" ;;
    esac
}

# NOTE: bmcweb emits compact (single-line, no-whitespace) JSON — these helpers
# parse it with grep/sed only, no python/jq required, so they work on the
# busybox utils available on the BMC.

# NOTE: every helper below is defensive against `set -euo pipefail` from the
# script header — grep no-match must not abort the script. Pipelines that
# could fail are wrapped with `|| true` and the empty result drives a [FAIL]
# message instead of a silent exit.

# Helpers below all share this diagnostic GET. On every miss they show the
# HTTP code + a body snippet so the user can tell whether the response was
# empty / 4xx / 5xx / valid-but-missing-the-field.

# Fetches $SYS_URL, flattens body, records HTTP code + size. Always returns 0
# so set -e doesn't fire.
function fetch_sys() {
    local code
    code=$(bmcweb_curl GET "$SYS_URL" || true)
    local sz=0
    [ -f "$BMCWEB_BODY" ] && sz=$(wc -c <"$BMCWEB_BODY" 2>/dev/null || echo 0)
    BMCWEB_LAST_CODE="$code"
    BMCWEB_LAST_SIZE="$sz"
    flatten_body
}

# Dump diagnostic info on a check failure.
function debug_dump() {
    local flat_sz=0
    [ -f "$BMCWEB_BODY_FLAT" ] && flat_sz=$(wc -c <"$BMCWEB_BODY_FLAT" 2>/dev/null || echo 0)
    info "GET $SYS_URL → HTTP $BMCWEB_LAST_CODE, body $BMCWEB_LAST_SIZE bytes (flat $flat_sz)"
    if [ "${BMCWEB_LAST_SIZE:-0}" -gt 0 ]; then
        # busybox head doesn't always support -c; dd works everywhere.
        info "body[:400]: $(dd if="$BMCWEB_BODY" bs=400 count=1 2>/dev/null)"
        # Also dump first 400 bytes of the flat file so we can see whether
        # flatten worked (should be one long line, no embedded newlines).
        info "flat[:400]: $(dd if="$BMCWEB_BODY_FLAT" bs=400 count=1 2>/dev/null)"
        cp -f "$BMCWEB_BODY" /tmp/test_prebootdiag_last_failed_body 2>/dev/null && \
            info "(full body saved to /tmp/test_prebootdiag_last_failed_body)"
    fi
}

# Map daemon DiagStatus uint8 → expected Redfish string and verify Redfish reflects it.
function check_redfish_status() {
    local n=$1 want=""
    case "$n" in
        0) want="Inprogress" ;;
        1) want="RecoveryMode" ;;
        2) want="Completed" ;;
        3) want="Abort" ;;
        4) want="Not Started" ;;
        5) want="TestRunning" ;;
        *)
            fail "check_redfish_status: unknown N=$n"
            return
            ;;
    esac
    fetch_sys
    # Patterns run against the flattened (whitespace-collapsed) body so they
    # match both compact and pretty-printed bmcweb output.
    local raw got
    raw=$(grep -o '"DiagStatus": *"[^"]*"' "$BMCWEB_BODY_FLAT" 2>/dev/null | head -n 1 || true)
    got=$(printf '%s' "$raw" | sed 's/^"DiagStatus": *"\(.*\)"$/\1/' || true)
    if [ "$got" = "$want" ]; then
        pass "Redfish DiagStatus=\"$got\" matches D-Bus uint8=$n"
    else
        fail "Redfish DiagStatus=\"$got\" expected \"$want\" for D-Bus uint8=$n"
        debug_dump
    fi
}

# Verify Redfish DiagMode matches expected ("true" / "false").
function check_redfish_diag_mode() {
    local want=$1
    fetch_sys
    local raw got
    raw=$(grep -o '"DiagMode": *\(true\|false\)' "$BMCWEB_BODY_FLAT" 2>/dev/null | head -n 1 || true)
    got=$(printf '%s' "$raw" | sed 's/^"DiagMode": *//' || true)
    if [ "$got" = "$want" ]; then
        pass "Redfish DiagMode=$want"
    else
        fail "Redfish DiagMode=$got, expected $want"
        debug_dump
    fi
}

# Verify ProcessorDiagResult is present and non-empty in the Redfish view.
function check_redfish_result_present() {
    fetch_sys
    # Non-empty array in flattened form starts with "[ {".
    if grep -q '"ProcessorDiagResult": *\[ *{' "$BMCWEB_BODY_FLAT" 2>/dev/null; then
        pass "Redfish ProcessorDiagResult populated"
    else
        fail "Redfish ProcessorDiagResult empty (or missing)"
        debug_dump
    fi
}

# Verify the seeded Sys+Tid configs are visible via Redfish GET. Note: bmcweb
# emits SysConfig as a JSON object (the daemon writes the property as a single
# object string), and TidConfig as a JSON array — handle both shapes.
function check_redfish_configs_visible() {
    fetch_sys
    local sc_present tc_present
    if grep -q '"ProcessorDiagSysConfig": *[{[]' "$BMCWEB_BODY_FLAT" 2>/dev/null; then
        sc_present=1
    else
        sc_present=0
    fi
    if grep -q '"ProcessorDiagTidConfig": *\[ *{' "$BMCWEB_BODY_FLAT" 2>/dev/null; then
        tc_present=1
    else
        tc_present=0
    fi
    if [ "$sc_present" = 1 ]; then
        info "SysConfig present in Redfish view"
    else
        info "SysConfig NOT in Redfish view"
    fi
    if [ "$tc_present" = 1 ]; then
        info "TidConfig present in Redfish view"
    else
        info "TidConfig NOT in Redfish view"
    fi
    if [ "$sc_present" = 1 ] && [ "$tc_present" = 1 ]; then
        pass "Redfish exposes seeded Sys+Tid configs"
    else
        fail "Redfish missing Sys/Tid configs"
        debug_dump
    fi
}

# Set Enable=true on prebootdiag (starts a new diagnostic session).
function enable_service() {
    busctl set-property "$PREBOOTDIAG_SVC" "$PREBOOTDIAG_OBJ" \
        "$ENABLE_IFACE" Enabled b true 2>&1
}

# Set Enable=false on prebootdiag (aborts any running session; no-op otherwise).
function disable_service() {
    busctl set-property "$PREBOOTDIAG_SVC" "$PREBOOTDIAG_OBJ" \
        "$ENABLE_IFACE" Enabled b false 2>&1
}

# Enable/disable via the Redfish POST handler — drives the full bmcweb path
# (preconditions + Settings DiagMode write + ObjectMapper-resolved
# setPreBootDiagEnabled). Use this for tests that exercise the production flow
# and depend on bmcweb's view (Settings DiagMode, Redfish GET shape).
#
# Don't use these in tests that intentionally set up unusual pre-state to
# exercise the daemon's own abort logic — bmcweb's preconditions would
# intercept and return 412/409 before the daemon ever sees the request.
# Those tests (Step 12, Step 13) keep enable_service.
function enable_via_redfish() {
    bmcweb_curl POST "$PROC_URL/ProcessorDiagCapabilities" \
        '{"ProcessorDiagCapabilities":{"DiagMode":"Enable"}}' >/dev/null
}
function disable_via_redfish() {
    bmcweb_curl POST "$PROC_URL/ProcessorDiagCapabilities" \
        '{"ProcessorDiagCapabilities":{"DiagMode":"Disable"}}' >/dev/null
}

# Push a state directly to prebootdiag via the App.Notify method.
# Method on com.nvidia.PreBootDiag.App at /com/nvidia/prebootdiag.
# Usage: send_state <state> [payload]
function send_state() {
    local state=$1
    local payload=${2:-""}
    busctl call "$PREBOOTDIAG_SVC" "$PREBOOTDIAG_OBJ" "$APP_IFACE" \
        Notify ss "$state" "$payload" 2>&1
}

# Gate helper — simulates the postcode-manager producer (which lives
# outside both nsmd and prebootdiag) sending a specific post code via
# the generic PostCodeReceived state plus a JSON payload.
function send_postcode() {
    local name=$1
    send_state "$S_POSTCODE" "{\"PostCodeName\":\"$name\"}"
}

# Drive the two ordered post codes so listenForEvents starts accepting
# NSM events. The second send wakes the coroutine; a small sleep gives
# it time to move to the event loop before the next push.
function pass_gates() {
    send_postcode "$PC_PSC_FMC" >/dev/null 2>&1 || true
    send_postcode "$PC_MB2"     >/dev/null 2>&1 || true
    sleep 1
}

# NSM event triggers — these drive the nsmd mock responder, which in turn
# pushes App.Notify into prebootdiag (plus creates the Config endpoint
# that prebootdiag calls back for Cmd 0x80/0x81). They exercise the full
# end-to-end nsmd ⇄ prebootdiag loop.
function trigger_system_config() {
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagGetSystemConfigEvent yby 14 true 0
}

function trigger_tid_config() {
    local tid=${1:-1}
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagGetTidConfigEvent yby 14 true "$tid"
}

function trigger_heartbeat() {
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagSetFlowControlEvent yy 14 1
}

function trigger_result() {
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagSetTestResultEvent ybyqay 14 true 1 0 0
}

function trigger_result_with() {
    local tid=${1:-1} result=${2:-0}
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagSetTestResultEvent ybyqay 14 true "$tid" "$result" 0
}

function trigger_finished() {
    busctl call "$NSM_MOCK_SVC" "$NSM_MOCK_OBJ" "$NSM_MOCK_IFACE" \
        genDiagSetFlowControlEvent yy 14 2
}

function seed_configs_with() {
    local sys_config=$1 tid_config=$2
    busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
        DiagSystemConfig s "$sys_config"
    busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
        DiagConfig s "$tid_config"
}

function seed_configs() {
    seed_configs_with "$DIAG_SYS_CONFIG" "$DIAG_CONFIG"
}

# Restore known-good settings state between tests.
function reset_settings() {
    busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
        DiagStatus y 4
    busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
        DiagResult s "[]"
}

# Start from a clean slate: defensively cancel any running session, then
# re-seed configs and reset DiagStatus / DiagResult. The disable_service
# call is mostly defensive now — runDiagnosticSession resets Enabled=false
# at session end, so a stale Enabled=true after a failure is no longer a
# concern. Still useful when a session is mid-flight.
function ensure_fresh_session() {
    disable_service >/dev/null 2>&1 || true
    sleep 1
    seed_configs
    reset_settings
}

# ─────────────────────────────────────────────
# Pre-flight checks
# ─────────────────────────────────────────────

step "Pre-flight: verify services are on the bus"

for svc in "$PREBOOTDIAG_SVC" "$SETTINGS_SVC" "$NSM_SVC"; do
    if busctl status "$svc" >/dev/null 2>&1; then
        pass "$svc is running"
    else
        fail "$svc is NOT on the bus — aborting"
        exit 1
    fi
done

# bmcweb reachability — RF_01, RF_02. If this fails, every Redfish-layer
# assertion below will fail too, but the daemon-driven sections still run.
if curl -s -o /dev/null -w '%{http_code}' "$BMCWEB_BASE/redfish/v1" | grep -q '^200$'; then
    pass "RF_01 bmcweb ServiceRoot reachable at $BMCWEB_BASE"
else
    fail "RF_01 bmcweb not reachable at $BMCWEB_BASE — Redfish-layer assertions will all fail"
fi
if curl -s -o /dev/null -w '%{http_code}' "$SYS_URL" | grep -q '^200$'; then
    pass "RF_02 ComputerSystem $SYS_ID reachable"
else
    fail "RF_02 ComputerSystem $SYS_ID not reachable"
fi

# ═══════════════════════════════════════════════════════
# GROUP 0: bmcweb HTTP-layer validation
#   Standalone Redfish tests that don't touch daemon state (or only read it).
#   Maps to RF_03, RF_04-06, RF_07, RF_10-15, RF_20-29, RF_37-41 in the IT plan.
# ═══════════════════════════════════════════════════════

step "bmcweb: ActionInfo endpoints reachable (RF_04, RF_05, RF_06)"
bmcweb_assert_status RF_04 "200" GET "$PROC_URL/ProcessorDiagCapabilitiesActionInfo"
bmcweb_assert_status RF_05 "200" GET "$PROC_URL/ProcessorDiagSysConfigActionInfo"
bmcweb_assert_status RF_06 "200" GET "$PROC_URL/ProcessorDiagTidConfigActionInfo"

step "bmcweb: Disable in clean state (RF_03, RF_36)"
bmcweb_assert_status RF_03 "200 204" POST "$PROC_URL/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{"DiagMode":"Disable"}}'
bmcweb_assert_status RF_36 "200 204" POST "$PROC_URL/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{"DiagMode":"Disable"}}'

step "bmcweb: Enable rejected with 412 when DiagConfig empty (RF_07)"
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagConfig s '[]'
bmcweb_assert_status RF_07 "412" POST "$PROC_URL/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{"DiagMode":"Enable"}}'

step "bmcweb: SysConfig payload validation (RF_10-15)"
bmcweb_assert_status RF_10 "400" POST "$PROC_URL/ProcessorDiagSysConfig" \
    '{"ProcessorDiagSysConfig":[{"ConfigType":5,"TestDuration":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_11 "400" POST "$PROC_URL/ProcessorDiagSysConfig" \
    '{"ProcessorDiagSysConfig":[{"ConfigType":1,"TestDuration":256,"DynamicData":[0]}]}'
bmcweb_assert_status RF_12 "400" POST "$PROC_URL/ProcessorDiagSysConfig" \
    '{"ProcessorDiagSysConfig":[{"ConfigType":1,"TestDuration":1,"DynamicData":[300]}]}'
bmcweb_assert_status RF_13 "400" POST "$PROC_URL/ProcessorDiagSysConfig" \
    '{"ProcessorDiagSysConfig":"not_an_array"}'
bmcweb_assert_status RF_14 "400" POST "$PROC_URL/ProcessorDiagSysConfig" \
    '{"ProcessorDiagSysConfig":[{"ConfigType":1}]}'
bmcweb_assert_status RF_15 "400 415" POST "$PROC_URL/ProcessorDiagSysConfig" \
    'not json at all' 'Content-Type: text/plain'

step "bmcweb: TidConfig payload validation (RF_20-29)"
bmcweb_assert_status RF_20 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicDataSize":5,"DynamicData":[0,0,0]}]}'
bmcweb_assert_status RF_21 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]},{"Tid":1,"TestDuration":2,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_22 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":256,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_23 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":256,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_24 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":1,"Loops":65536,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_25 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":256,"DynamicDataSize":1,"DynamicData":[0]}]}'
# RF_26: spreadsheet says >244 → 400, but cap was corrected to 255 in bmcweb MR 7438.
# Use 256 to actually trigger 400. Build the 256-element zero array via awk
# (no python/jq dependency — this script runs on busybox).
RF26_DATA=$(awk 'BEGIN{for(i=0;i<256;i++) printf (i?",0":"0")}')
RF26_BODY="{\"ProcessorDiagTidConfig\":[{\"Tid\":1,\"TestDuration\":1,\"Loops\":0,\"LogLevel\":0,\"DynamicDataSize\":256,\"DynamicData\":[$RF26_DATA]}]}"
bmcweb_assert_status RF_26 "400" POST "$PROC_URL/ProcessorDiagTidConfig" "$RF26_BODY"
bmcweb_assert_status RF_27 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[300]}]}'
bmcweb_assert_status RF_28 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":[{"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicDataSize":1,"DynamicData":[0]}]}'
bmcweb_assert_status RF_29 "400" POST "$PROC_URL/ProcessorDiagTidConfig" \
    '{"ProcessorDiagTidConfig":"not_an_array"}'

step "bmcweb: Method/path negatives (RF_37-41)"
bmcweb_assert_status RF_37 "400" POST "$PROC_URL/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{"DiagMode":"InvalidValue"}}'
bmcweb_assert_status RF_38 "400" POST "$PROC_URL/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{}}'
bmcweb_assert_status RF_39 "404 405" PUT "$PROC_URL/ProcessorDiagCapabilities"
bmcweb_assert_status RF_40 "404" POST \
    "$BMCWEB_BASE/redfish/v1/Systems/NONEXISTENT/Oem/Nvidia/ProcessorDiagCapabilities" \
    '{"ProcessorDiagCapabilities":{"DiagMode":"Disable"}}'
bmcweb_assert_status RF_41 "400 415" POST "$PROC_URL/ProcessorDiagTidConfig" \
    'not json at all' 'Content-Type: text/plain'

# ─────────────────────────────────────────────
# Setup: seed configs and reset state
# ─────────────────────────────────────────────
step "Seed DiagSystemConfig in Settings"
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
    DiagSystemConfig s "$DIAG_SYS_CONFIG"
info "DiagSystemConfig set"

step "Seed DiagConfig (TID config) in Settings"
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" \
    DiagConfig s "$DIAG_CONFIG"
info "DiagConfig set"

step "Verify config readback after seeding"
check_property_contains DiagSystemConfig "ConfigType" "DiagSystemConfig has ConfigType"
check_property_contains DiagSystemConfig "TestDuration" "DiagSystemConfig has TestDuration"
check_property_contains DiagSystemConfig "DynamicData" "DiagSystemConfig has DynamicData"
check_property_contains DiagConfig "Tid" "DiagConfig has Tid"
check_property_contains DiagConfig "Loops" "DiagConfig has Loops"
check_property_contains DiagConfig "DynamicData" "DiagConfig has DynamicData"
check_redfish_configs_visible

ensure_fresh_session
info "Initial state reset complete"
check_redfish_diag_mode false

# ═══════════════════════════════════════════════════════
# GROUP A: Precondition / Guard Tests
# ═══════════════════════════════════════════════════════

step "Negative: Notify without active session is rejected"
output=$(send_postcode "$PC_PSC_FMC") && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
    pass "Notify correctly rejected — no session in progress"
    info "$output"
else
    fail "Notify should have been rejected but succeeded"
fi

mark_journal
step "Negative: Missing DiagConfig triggers abort (SADD TC-05)"
# hasDiagConfig() checks the property value — empty or "[]" returns false.
ensure_fresh_session
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagConfig s ""
reset_settings
enable_service >/dev/null 2>&1 || true
wait_settle
check_status 3 # Abort — hasDiagConfig returned false
check_journal_for "No DiagConfig" "Journal logs missing DiagConfig"
# Restore
ensure_fresh_session

mark_journal
step "Negative: DiagStatus already InProgress rejects start (SADD TC-07)"
# Pre-set DiagStatus to InProgress so the async precondition check fails.
# setEnabled(true) returns successfully (sequenceRunning becomes true), but
# the coroutine detects InProgress and aborts the session.
ensure_fresh_session
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagStatus y 0
enable_service >/dev/null 2>&1 || true
wait_settle
check_status 3 # Abort — coroutine detected "already running" and aborted
check_journal_for "already running" "Journal logs 'already running' failure"
ensure_fresh_session

mark_journal
step "Negative: MB2 post code before PSC-FMC aborts session (out of order)"
# With both gates required in order, MB2 arriving first must trigger the
# standard recovery flow: -EPROTO to caller, DiagStatus→Abort, error log
# carrying the out-of-order reason.
ensure_fresh_session
enable_service >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress
output=$(send_postcode "$PC_MB2") && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
    pass "Out-of-order MB2 rejected to caller"
    info "$output"
else
    fail "Out-of-order MB2 should have been rejected"
fi
wait_settle
check_status 3 # Abort — recovery flow ran
check_journal_for "Out-of-order" "Journal logs out-of-order abort"
ensure_fresh_session

# ═══════════════════════════════════════════════════════
# HAPPY PATH with full DiagStatus verification
# (SADD TC-06, TC-15, TC-16, TC-20, TC-22, TC-27)
# ═══════════════════════════════════════════════════════

step "Enable prebootdiag -> session starts"
ensure_fresh_session
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress
check_redfish_status 0 # RF_43 (Inprogress)
check_redfish_diag_mode true
check_redfish_configs_visible # RF_32 (full diag data with DiagMode=true)

step "Send PSC_FMC then MB2 post codes -> enter event loop"
if send_postcode "$PC_PSC_FMC" >/dev/null 2>&1; then
    pass "PSC_FMC post code accepted"
else
    fail "PSC_FMC post code rejected"
fi
if send_postcode "$PC_MB2" >/dev/null 2>&1; then
    pass "MB2 post code accepted"
else
    fail "MB2 post code rejected"
fi
wait_settle
check_status 0 # Still InProgress, waiting for NSM events

step "Trigger getDiagSystemConfig -> SystemConfigRequested (SADD TC-15)"
trigger_system_config
wait_settle
# DiagStatus stays 0 (InProgress) until the first heartbeat flips to TestRunning.
check_status 0

step "Trigger getDiagTidConfig -> TIDConfigRequested (SADD TC-16)"
trigger_tid_config 1
wait_settle

step "Trigger heartbeat -> HeartbeatReceived"
trigger_heartbeat
wait_for_status 5 10 # TestRunning
check_redfish_status 5 # RF_48 (TestRunning)

step "Trigger test result -> ResultReceived (SADD TC-20)"
trigger_result
wait_for_status 2 10 # Completed
check_redfish_status 2 # RF_45 (Completed)

step "Verify DiagResult content (SADD FR-11, FR-13)"
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q '^s "'; then
    pass "DiagResult populated"
else
    fail "DiagResult not set"
fi
if echo "$result" | grep -q 'Tid'; then
    pass "DiagResult contains Tid field"
else
    fail "DiagResult missing Tid field"
fi
if echo "$result" | grep -q 'Result'; then
    pass "DiagResult contains Result field"
else
    fail "DiagResult missing Result field"
fi
check_redfish_result_present # RF_42 (DiagResult visible via Redfish GET)

step "Trigger finished -> SessionEnded (session end)"
trigger_finished
wait_for_status 4 10 # NotStarted
check_redfish_status 4 # RF_47 (Not Started)

# ─────────────────────────────────────────────
# Verify clean state after session
# ─────────────────────────────────────────────
step "Verify: can start a new diagnostic session after clean completion"
ensure_fresh_session
if enable_via_redfish >/dev/null 2>&1; then
    wait_settle
    check_status 0 # InProgress
    pass "New session accepted — state correctly reset"
else
    fail "New session rejected"
fi
# Clean up: disable so any subsequent enable starts fresh.
disable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_redfish_diag_mode false # RF_57 (DiagMode=false after lifecycle)

# ═══════════════════════════════════════════════════════
# GROUP B: NSM State Machine Variations
# ═══════════════════════════════════════════════════════

step "Multiple heartbeats before result (SADD FR-09)"
ensure_fresh_session
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
trigger_system_config
wait_settle
trigger_tid_config 1
wait_settle
info "Sending heartbeat 1 of 3"
trigger_heartbeat
# Polling wait — the prior config-push events may still be awaiting
# Async.Set completion when the heartbeat arrives, so the state
# transition to TestRunning can lag the trigger by several seconds.
wait_for_status 5 10 # TestRunning
info "Sending heartbeat 2 of 3"
trigger_heartbeat
wait_for_status 5 10 # TestRunning
info "Sending heartbeat 3 of 3"
trigger_heartbeat
wait_for_status 5 10 # TestRunning
trigger_result
wait_for_status 2 10 # Completed
trigger_finished
wait_for_status 4 10 # NotStarted
pass "Multiple heartbeats handled correctly"

step "Multiple TID config requests (SADD Flow 3 per-TID loop)"
ensure_fresh_session
# Seed a multi-TID config so both TID 1 and TID 2 requests are valid
seed_configs_with "$DIAG_SYS_CONFIG_2" "$DIAG_CONFIG_2"
reset_settings
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
trigger_system_config
wait_settle
info "Sending TIDConfig request for TID 1"
trigger_tid_config 1
wait_settle
info "Sending TIDConfig request for TID 2"
trigger_tid_config 2
wait_settle
trigger_heartbeat
wait_settle
trigger_result
wait_settle
trigger_finished
wait_for_status 4 10 # NotStarted
pass "Multiple TID config requests handled correctly"
# Restore default config for subsequent tests
seed_configs

# ═══════════════════════════════════════════════════════
# GROUP C: Failure & Recovery
# ═══════════════════════════════════════════════════════

step "Session reuse after abort (SADD Section 7.3)"
# Trigger abort via empty DiagConfig — session fails at hasDiagConfig check.
# Uses enable_service (busctl direct) to bypass bmcweb's 412 precondition; we
# specifically want the daemon's hasDiagConfig() abort path to fire.
ensure_fresh_session
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagConfig s ""
reset_settings
enable_service >/dev/null 2>&1 || true
wait_settle
check_status 3 # Abort
info "Session aborted as expected, now attempting recovery..."
# Restore config and state, then start a new session
ensure_fresh_session
if enable_via_redfish >/dev/null 2>&1; then
    wait_settle
    check_status 0 # InProgress
    pass "New session accepted after abort — recovery successful"
else
    fail "New session rejected after abort"
fi
# Clean up
disable_via_redfish >/dev/null 2>&1 || true
wait_settle

mark_journal
step "Error log verification on failure (SADD NFR-06)"
# Same as Step 28: use enable_service (busctl direct) — bmcweb's 412
# precondition would otherwise prevent the daemon from emitting the
# "No DiagConfig" journal entry we're asserting on.
ensure_fresh_session
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagConfig s ""
reset_settings
enable_service >/dev/null 2>&1 || true
wait_settle
wait_settle
check_journal_for "No DiagConfig" "prebootdiag logged failure with reason"
ensure_fresh_session

mark_journal
step "Missing TID config triggers abort (GAP 10 — SADD 7.3)"
ensure_fresh_session
# Seed DiagConfig with only TID 1, but CPU will request TID 99
seed_configs_with "$DIAG_SYS_CONFIG" '[{"Tid":1,"TestDuration":1,"Loops":0,"LogLevel":0,"DynamicData":[]}]'
reset_settings
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
trigger_system_config
wait_settle
# CPU requests TID 99 which doesn't exist in DiagConfig
trigger_tid_config 99
wait_for_status 3 10 # Abort — no matching TID config
check_journal_for "No DiagConfig found for requested TID" "Journal logs missing TID config"
pass "Missing TID config correctly triggered abort"

mark_journal
step "Disable mid-session cancels running session (GAP 7 — SADD 8.1.2)"
ensure_fresh_session
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
trigger_system_config
wait_settle
check_status 0 # InProgress
info "Calling disable_via_redfish to abort..."
disable_via_redfish >/dev/null 2>&1 || true
wait_settle
wait_settle
check_status 3 # Abort — session aborted by user
check_journal_for "aborted by user" "Journal logs user-initiated abort"
pass "Disable correctly cancelled session"

# ═══════════════════════════════════════════════════════
# GROUP D: Alternate Config Sets (testplan.py coverage)
# ═══════════════════════════════════════════════════════

step "Per-TID config filtering and result accumulation (testplan.py Test 2)"
ensure_fresh_session
seed_configs_with "$DIAG_SYS_CONFIG_2" "$DIAG_CONFIG_2"
check_property_contains DiagSystemConfig "TestDuration" "Multi-TID sys config seeded"
check_property_contains DiagConfig "Tid" "Multi-TID config seeded"
reset_settings
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress
pass_gates
trigger_system_config
wait_settle
# CPU requests TID configs one at a time (per spec Section 5.2, 7.5)
trigger_tid_config 1
wait_settle
trigger_tid_config 2
wait_settle
trigger_tid_config 4
wait_settle
trigger_heartbeat
wait_for_status 5 10 # TestRunning
# CPU reports results per TID — each should be appended to DiagResult array
info "Sending result for TID 1..."
trigger_result_with 1 0
wait_settle
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult after TID 1: $result"
if echo "$result" | grep -q 'Tid.*:1'; then
    pass "DiagResult contains TID 1 result"
else
    fail "DiagResult missing TID 1 result"
fi
info "Sending result for TID 2..."
trigger_result_with 2 0
wait_settle
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult after TID 2: $result"
if echo "$result" | grep -q 'Tid.*:1' && echo "$result" | grep -q 'Tid.*:2'; then
    pass "DiagResult accumulated TID 1 + TID 2"
else
    fail "DiagResult not accumulating — missing TID 1 or TID 2"
fi
info "Sending result for TID 4..."
trigger_result_with 4 0
wait_settle
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult after TID 4: $result"
if echo "$result" | grep -q 'Tid.*:1' && echo "$result" | grep -q 'Tid.*:2' && echo "$result" | grep -q 'Tid.*:4'; then
    pass "DiagResult accumulated all 3 TID results"
else
    fail "DiagResult missing results — not all 3 TIDs present"
fi
trigger_finished
wait_for_status 4 10 # NotStarted
pass "Per-TID config filtering and result accumulation completed"

step "Error result from firmware (testplan.py Test 1 — non-zero Result code)"
# Use valid configs so the session reaches the result-receiving stage. The
# "error" semantic is exercised via trigger_result_with 1 15 — the Result
# field carries the firmware error code while the session itself completes
# normally (service treats any result as completion).
ensure_fresh_session
seed_configs
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress
pass_gates
trigger_system_config
wait_settle
trigger_tid_config 1
wait_settle
trigger_heartbeat
wait_settle
# Firmware returns error result (15 = 0xf) for invalid duration level
trigger_result_with 1 15
wait_for_status 2 10 # Completed (service treats any result as completion)
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q 'Result'; then
    pass "DiagResult populated with error result"
else
    fail "DiagResult not set after error result"
fi
trigger_finished
wait_for_status 4 10 # NotStarted
pass "Error result handled gracefully — session completed and reset"

mark_journal
step "Invalid SystemConfig rejected by nsmd aborts session (DIAG_SYS_CONFIG_ERR)"
# nsmd validates the SystemConfig (TestDuration must be in [0, 3]) and
# rejects TestDuration=4 with InvalidArgument. prebootdiag treats the NSM
# push failure as an abort condition (per spec — silent skip was the prior
# behavior, flagged by review). Session must land at DiagStatus=3 (Abort)
# and the journal must surface the failure.
ensure_fresh_session
seed_configs_with "$DIAG_SYS_CONFIG_ERR" "$DIAG_CONFIG"
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress
pass_gates
trigger_system_config
wait_for_status 3 10 # Abort — nsmd rejected SystemConfig
check_journal_for "SystemConfig push failed" "Journal logs SystemConfig push failure"
pass "Invalid SystemConfig correctly aborted session"

# Restore default configs for remaining tests
seed_configs

# ═══════════════════════════════════════════════════════
# GROUP E: Stress tests (rapid-fire signals + heartbeat timeout)
#
# Tests the prebootdiag service's event channel buffering under pressure and
# verifies the heartbeat timeout mechanism.
# ═══════════════════════════════════════════════════════

step "Rapid-fire: full session back-to-back (event buffering stress)"
ensure_fresh_session
seed_configs
enable_via_redfish >/dev/null 2>&1 || true
wait_settle # ensure listenForEvents is ready past the gates
pass_gates
info "Firing all NSM state transitions back-to-back..."
trigger_system_config
trigger_tid_config 1
trigger_heartbeat
trigger_result_with 1 0
sleep 1
trigger_finished
# Polling wait — rapid-fire events queue up while the service awaits
# Async.Set completion on each config push (can take 4–5s under load).
wait_for_status 4 15 # NotStarted (session completed)
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q 'Tid'; then
    pass "Rapid-fire session produced results with Tid fields"
else
    fail "Rapid-fire session did not produce results"
fi
pass "Rapid-fire full session completed"

step "Rapid-fire: multi-TID config requests back-to-back"
ensure_fresh_session
seed_configs_with "$DIAG_SYS_CONFIG_2" "$DIAG_CONFIG_2"
reset_settings
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
info "Firing config + 3 TID requests + heartbeat + result + finish..."
trigger_system_config
trigger_tid_config 1
trigger_tid_config 2
trigger_tid_config 4
trigger_heartbeat
trigger_result_with 1 0
sleep 1
trigger_finished
wait_for_status 4 15
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q 'Tid'; then
    pass "Rapid-fire multi-TID session produced results"
else
    fail "Rapid-fire multi-TID session did not produce results"
fi
pass "Rapid-fire multi-TID session completed"

step "Rapid-fire: burst of 5 heartbeats then result (SADD FR-09 stress)"
ensure_fresh_session
seed_configs
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
info "Firing config + 5 rapid heartbeats + result + finish..."
trigger_system_config
trigger_tid_config 1
trigger_heartbeat
trigger_heartbeat
trigger_heartbeat
trigger_heartbeat
trigger_heartbeat
trigger_result_with 1 0
sleep 1
trigger_finished
wait_for_status 4 15
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q 'Tid'; then
    pass "Burst heartbeat session produced results"
else
    fail "Burst heartbeat session did not produce results"
fi
pass "Rapid-fire heartbeat burst completed"

step "Rapid-fire: error result with burst heartbeats"
# Valid configs (Sys + TID) so the session runs end-to-end. The error
# code 15 in the Result field exercises the non-zero-result handling.
ensure_fresh_session
seed_configs
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
info "Firing config + heartbeat burst + error result + finish..."
trigger_system_config
trigger_tid_config 1
trigger_heartbeat
trigger_heartbeat
trigger_heartbeat
trigger_result_with 1 15 # error code 15 (0xf) — non-zero Result
sleep 1
trigger_finished
# Polling wait — rapid-fire events queue up while the service awaits
# Async.Set completion on the config push (which can take 4–5s under
# load); single sleep+check races the session-end transition.
wait_for_status 4 15
result=$(busctl get-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagResult 2>&1) || true
echo "  DiagResult: $result"
if echo "$result" | grep -q 'Result'; then
    pass "Error result stored after heartbeat burst"
else
    fail "No error result after heartbeat burst"
fi
pass "Rapid-fire error with heartbeat burst completed"

# Restore default configs
seed_configs

mark_journal
step "PSC-FMC post-code timeout: no PSC-FMC within 30s aborts session (SADD TC-18)"
# Phase 1 of waitForPostCodes (pscFmcPostCodeWaitTimeout = 30s): with the
# session enabled and no post codes pushed, the daemon must abort once the
# 30s window elapses. MB2's 15min phase 2 is never reached.
# The MB2 timeout itself isn't bench-tested (15min is impractical) — the
# gtest fixture exercises it with a shortened timeout instead.
ensure_fresh_session
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
check_status 0 # InProgress, waiting for PSC-FMC
info "Waiting 35s with no post codes for PSC-FMC timeout (30s + margin)..."
sleep 35
check_status 3 # Abort — PSC-FMC didn't arrive
check_journal_for "Timed out waiting for PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG" \
    "Journal logs PSC-FMC post-code timeout"
pass "PSC-FMC post-code timeout correctly aborted session"

mark_journal
step "Event-idle timeout: no events within 3 minutes aborts session (SADD FR-09, 7.4)"
ensure_fresh_session
seed_configs
enable_via_redfish >/dev/null 2>&1 || true
wait_settle
pass_gates
info "Sending config + heartbeat, then waiting 185s with no further events..."
trigger_system_config
wait_settle
trigger_tid_config 1
wait_settle
trigger_heartbeat
# Use a polling wait — the prebootdiag service processes events serially
# and may still be awaiting Async.Set completion for the TIDConfig push
# when the heartbeat arrives. wait_for_status retries for up to 10s,
# which comfortably covers the worst-case Async.Set latency.
wait_for_status 5 10 # TestRunning
info "Waiting 185s with no further events for event-idle timeout (3min + margin)..."
sleep 185      # eventTimeout(3min) + margin
check_status 3 # Abort — no events received within eventTimeout
check_journal_for "No NSM events received" "Journal logs event-idle timeout"
pass "Event-idle timeout correctly aborted session"

# ═══════════════════════════════════════════════════════
# Final: Notify while disabled
# ═══════════════════════════════════════════════════════

step "Failure path: with service disabled, Notify is rejected"
ensure_fresh_session
output=$(send_state "$S_SYSCFG" '{"ConfigType":0,"Eid":14}') && rc=0 || rc=$?
if [ "$rc" -ne 0 ]; then
    pass "Notify correctly rejected while disabled"
    info "$output"
else
    fail "Notify should have been rejected while disabled"
fi

# Also verify that NSM events flowing through nsmd while disabled do not
# create a spurious session — nsmd will still push App.Notify, and
# prebootdiag will reject them.
mark_journal
info "Triggering an NSM event while prebootdiag is disabled..."
trigger_system_config 2>/dev/null || true
wait_settle
info "Event should have been ignored by prebootdiag (check journal)"

# ═══════════════════════════════════════════════════════
# GROUP Z: Redfish DiagStatus mapping for daemon-only states
#   RF_44 (RecoveryMode = 1) and RF_46 (Abort = 3) aren't naturally produced
#   by the happy path; flip via busctl and verify Redfish reflects the string.
# ═══════════════════════════════════════════════════════
step "bmcweb: DiagStatus mapping for RecoveryMode and Abort (RF_44, RF_46)"
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagStatus y 1
sleep 0.3
check_redfish_status 1
busctl set-property "$SETTINGS_SVC" "$DIAG_OBJ" "$DIAG_IFACE" DiagStatus y 3
sleep 0.3
check_redfish_status 3
ensure_fresh_session

# ─────────────────────────────────────────────
# Done
# ─────────────────────────────────────────────
echo ""
echo -e "${GREEN}========================================${NC}"
if [ "$FAILURES" -gt 0 ]; then
    echo -e "${RED}  Test run complete: ${FAILURES} failure(s)${NC}"
else
    echo -e "${GREEN}  Test run complete: all tests passed${NC}"
fi
echo -e "${GREEN}  Check journal for details:${NC}"
echo -e "${GREEN}  journalctl -u ${JOURNAL_UNIT} --since '5 min ago'${NC}"
echo -e "${GREEN}========================================${NC}"

[ "$FAILURES" -eq 0 ]

journalctl -u nsmd && journalctl -u nsmMockResponder14 && journalctl -u com.nvidia.PreBootDiag
