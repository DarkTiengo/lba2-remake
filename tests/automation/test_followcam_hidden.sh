#!/usr/bin/env bash
# A hero the scenery hides brings the boom in; it does not throw the player's camera away.
#
# When the renderer finds the followed object with no pixel of it left (DrawRecover), the engine
# cuts the camera straight back behind him: CameraCenter(1) zeroes the pan, resets the pitch and
# the distance to the scene's defaults, and the frame is drawn again (SOURCES/OBJECT.CPP). That
# is right for a camera that was behind him anyway. With the free camera it throws away the angle
# and the tilt the player is holding, and where the scenery is broken up it fires again and
# again -- the rocks and the house by the cow on Citadel Island are where this was reported from.
#
# The hero is put beside the known building on Desert Island (the same one the decor fixture
# uses), the camera is tilted down and orbited until the building stands between it and him.
# `cam_decor` is off in both runs: the scenery clearance would stop the boom short of the
# building and he would never be hidden, which is the point of that feature and the opposite of
# what this one needs.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_hidden
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Desert Island.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

TILT=200 # units of AlphaCam in one frame: the tilt itself moves 10 at most

pulled="$(mktemp)"; cut="$(mktemp)"
trap 'rm -f "$pulled" "$cut"' EXIT

orbit_behind_the_building() { # <log> <cam_hidden>
    ctl_headless --load "$SAVE" --fixed-dt 16 \
        --exec "cam_follow 1; cam_hold_angle 1; camtrace 1; cam_decor 0; cam_hidden $2" \
        --exec-at 10 "teleport 16000 1360 10500" \
        --exec-at 30 "camnudge 0 -40 40" \
        --exec-at 90 "camnudge 8 0 150" \
        --tick 260 --exit > "$1" 2>&1 || fail "engine run failed: exit $?"
    grep -q "^\[INFO\] \[cam\]" "$1" || fail "no camera trace in the run"
}

# The cut shows up as the player's tilt being replaced by the scene's default in one frame.
tilt_thrown_away() {
    cam_col "$1" alpha | awk -v t="$TILT" '
        NR > 1 { d = $1 - prev; if (d < 0) d = -d; if (d > t) n++ }
        { prev = $1 }
        END { print n + 0 }'
}

orbit_behind_the_building "$cut" 0
[ "$(tilt_thrown_away "$cut")" -gt 0 ] \
    || fail "the classic cut never fired in this orbit: the fixture is not testing anything"

orbit_behind_the_building "$pulled" 1
thrown="$(tilt_thrown_away "$pulled")"
[ "$thrown" -eq 0 ] \
    || fail "the player's tilt was thrown away on $thrown frame(s) with cam_hidden on: the boom is not taking it"

# And the boom is what took it: the pull is FOLLOW_CAM_HIDDEN_PULL a frame, far past the spring's
# own 200, so a step that size downward is this feature and nothing else.
dipped="$(cam_col "$pulled" dist | awk '
    NR > 1 && $1 - prev <= -700 { n++ }
    { prev = $1 }
    END { print n + 0 }')"
[ "$dipped" -gt 0 ] \
    || fail "the boom never came in: the hero was hidden but nothing answered it"

pass
