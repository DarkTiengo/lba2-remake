#!/usr/bin/env bash
# A hero the scenery hides brings the boom in; it does not whip the view round.
#
# When the renderer finds the followed object with no pixel of it left (DrawRecover), the engine
# cuts the camera straight back behind him (CameraCenter(1), SOURCES/OBJECT.CPP). That is right
# for a camera that was behind him anyway; with the free camera it throws away the angle the
# player is holding, and among broken-up scenery -- the rocks and the house by the cow on Citadel
# Island, which is where this was reported from -- it fires over and over, each time whipping the
# view a third of the way round the world in a single frame.
#
# The hero is walked through that cube and the camera's own angle is watched for single-frame
# jumps outside camera zones (inside one the zone decides, and its cuts are authored). With
# `cam_hidden` the boom comes in instead and there are none; with it off the classic cut is back
# and the jump returns, which is what keeps this fixture honest.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_hidden
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Anon1.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

WHIP=120 # units of beta in one frame: more than the orbit or the lazy follow ever steps

pulled="$(mktemp)"; cut="$(mktemp)"
trap 'rm -f "$pulled" "$cut"' EXIT

walk_past_the_rocks() { # <log> <cam_hidden>
    ctl_headless --load "$SAVE" --fixed-dt 16 \
        --exec-at 4 "cube 49" \
        --exec-at 8 "cam_follow 1; cam_hold_angle 1; camtrace 1; cam_hidden $2" \
        --exec-at 20 "input up 120" \
        --exec-at 150 "input left 40" \
        --exec-at 200 "input up 120" \
        --tick 340 --exit > "$1" 2>&1 || fail "engine run failed: exit $?"
    grep -q "^\[INFO\] \[cam\]" "$1" || fail "no camera trace in the run"
}

# Single-frame beta steps taken while the camera zone stays as it was, so a zone's own cut is not
# counted as one.
whips() {
    paste <(cam_col "$1" beta) <(cam_col "$1" zone) | awk -v w="$WHIP" '
        NR > 1 && $2 == prevzone {
            d = $1 - prevbeta; if (d > 2048) d -= 4096; if (d < -2048) d += 4096
            if (d < 0) d = -d
            if (d > w) n++
        }
        { prevbeta = $1; prevzone = $2 }
        END { print n + 0 }'
}

walk_past_the_rocks "$cut" 0
[ "$(whips "$cut")" -gt 0 ] \
    || fail "the classic cut never fired in this walk: the fixture is not testing anything"

walk_past_the_rocks "$pulled" 1
whipped="$(whips "$pulled")"
[ "$whipped" -eq 0 ] \
    || fail "the view was whipped round on $whipped frame(s) with cam_hidden on: the boom is not taking it"

pass
