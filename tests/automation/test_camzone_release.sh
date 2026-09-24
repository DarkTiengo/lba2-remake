#!/usr/bin/env bash
# Leaving an authored shot eases the boom back; it does not cut to it.
#
# A camera zone writes its own boom into VueDistance (its Info6) and the Auto camera's update does
# not run while the shot owns the view. So the arm came back with the length it had before the
# shot, and the view cut to it in a single frame the moment the hero stepped out -- thousands of
# units, on every walk in and out of the shots around the cow on Citadel Island. The arm now
# adopts the shot's length while the shot holds it (SOURCES/PERSO.CPP), so the spring eases back
# to the player's distance instead.
#
# Entering a shot still cuts, and should: that cut is authored, and the classic camera makes it
# too. Only the release is asserted here.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=camzone_release
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Anon1.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

STEP=600 # FOLLOW_CAM_SPRING_RECOVER is 200 a frame; anything past this is a cut

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# Cube 49 is the cow's, where the shots cover the rocks and the house: this walk crosses two of
# them and leaves both.
ctl_headless --load "$SAVE" --fixed-dt 16 \
    --exec-at 4 "cube 49" \
    --exec-at 8 "cam_follow 1; cam_hold_angle 1; camtrace 1" \
    --exec-at 20 "input up 120" \
    --exec-at 150 "input left 40" \
    --exec-at 200 "input up 120" \
    --tick 340 --exit > "$log" 2>&1 || fail "engine run failed: exit $?"
grep -q "^\[INFO\] \[cam\]" "$log" || fail "no camera trace in the run"

releases="$(paste <(cam_col "$log" zone) <(cam_col "$log" dist) | awk '
    NR > 1 && prevzone == 1 && $1 == 0 { n++ }
    { prevzone = $1 }
    END { print n + 0 }')"
[ "$releases" -ge 2 ] \
    || fail "the walk left a shot only $releases time(s): the fixture is not testing anything"

cut="$(paste <(cam_col "$log" zone) <(cam_col "$log" dist) | awk -v s="$STEP" '
    NR > 1 && prevzone == 1 && $1 == 0 {
        d = $2 - prevdist; if (d < 0) d = -d
        if (d > s) { n++; if (d > worst) worst = d }
    }
    { prevzone = $1; prevdist = $2 }
    END { print n + 0, worst + 0 }')"
set -- $cut
[ "$1" -eq 0 ] \
    || fail "the boom jumped on $1 shot release(s), worst $2 units in one frame: it is cutting, not easing"

pass
