#!/usr/bin/env bash
# The boom does not reach through a building.
#
# The eased ground clearance ported the terrain half of the classic SearchCameraPos; the decor
# boxes it also tested (TestZVDecors) were never ported, so orbiting beside a building put the
# camera behind it — or inside it — with the hero hidden. The answer is the spring arm: where the
# sight line enters a box, the arm is shortened to stop just short of it.
#
# The hero is teleported beside a known building on Desert Island (the box at x 14116..18116,
# y 765..5931, z 7654..9653 in that cube) and the camera is tilted down and orbited past it, so
# the building is between the camera and the hero for part of the turn.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_decor
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Desert Island.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

REST=13750   # FOLLOW_CAM_INITIAL_DIST: the arm at rest, which is what an unblocked orbit keeps
PULLED=12000 # anything below this is the clearance bringing the boom in

blocked="$(mktemp)"; through="$(mktemp)"
trap 'rm -f "$blocked" "$through"' EXIT

orbit_past_the_building() { # <log> <cam_decor>
    ctl_headless --load "$SAVE" --fixed-dt 16 \
        --exec "cam_follow 1; cam_hold_angle 1; camtrace 1; cam_decor $2" \
        --exec-at 10 "teleport 16000 1360 10500" \
        --exec-at 30 "camnudge 0 -40 40" \
        --exec-at 90 "camnudge 8 0 150" \
        --tick 260 --exit > "$1" 2>&1 || fail "engine run failed: exit $?"
    grep -q "^\[INFO\] \[cam\]" "$1" || fail "no camera trace in the run"
}

shortest() { cam_col "$1" dist | sort -n | head -1; }

orbit_past_the_building "$blocked" 1
orbit_past_the_building "$through" 0

off="$(shortest "$through")"
[ "$off" -ge "$REST" ] \
    || fail "the arm moved ($off) with the clearance off: the run is not measuring what it thinks"

on="$(shortest "$blocked")"
[ "$on" -lt "$PULLED" ] \
    || fail "the arm stayed at $on past a building: the boom is still reaching through the scenery"

pass
