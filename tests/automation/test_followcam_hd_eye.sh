#!/usr/bin/env bash
# The scenery is drawn from the camera the Auto camera decided on.
#
# Two eyes are in play every exterior frame. UpdateFollowCameraExt places one to measure the
# clearances against (the ground lift, the ceiling drop) and AffGrilleExt re-derives another from
# the boom before drawing. Above 480 lines the HD recompose steepens the pitch of the first one
# only, so the two were hundreds of units apart, and the ceiling drop -- measured against the
# high eye, applied to the low one -- sank the view into the ground a little further every frame.
# The re-aim that followed made it worse: it went through SetTargetCamera, whose world Y and Z
# are swapped, so the drawn camera ended up turned most of the way round the world and pitched
# at the ground, with the scene's light swinging round with it (SetAngleCamera rebuilds it).
# On screen: orbiting a half turn left the hero off frame entirely.
#
# `camtrace` reports the drawn camera as [camdraw]; what it wants is want_alpha / want_beta.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_hd_eye
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

TOLERANCE=40 # FOLLOW_CAM_GROUND_SNAP: an ease settles within this

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Desert Island.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

# Desert Island, whose cloud ceiling sits a few hundred units over the boom's eye: high enough
# that the camera never really reaches it, low enough that the recomposed eye does. 720 lines, so
# the recompose is in play, and a half-turn orbit, which is where the drift had grown large
# enough to lose the hero off frame.
ctl_headless --load "$SAVE" --fixed-dt 16 --resolution 1280x720 \
    --exec "cam_follow 1; cam_hold_angle 1; camtrace 1" \
    --exec-at 20 "camnudge 52 0 40" \
    --tick 150 --exit > "$log" 2>&1 || fail "engine run failed: exit $?"
grep -q "^\[INFO\] \[camdraw\]" "$log" || fail "no camera trace in the run"

draw() { grep "^\[INFO\] \[camdraw\]" "$log" | awk -v want="$1" '
    { for (i = 1; i <= NF; i++) if ($i == want) print $(i + 1) }'; }

frames="$(draw alpha | wc -l)"
[ "$frames" -gt 60 ] || fail "only $frames drawn frames traced: the run is not measuring anything"

# The recompose has to be engaged, or this fixture passes at any resolution for the wrong reason:
# its distance gain pulls the boom in from the 13750 it rests at.
pulled="$(draw dist | awk '$1 < 13250 { n++ } END { print n + 0 }')"
[ "$pulled" -gt 0 ] \
    || fail "the boom was never pulled in: the HD recompose is not engaged, so nothing is tested"

for field in alpha beta; do
    off="$(paste <(draw "$field") <(draw "want_$field") | awk -v t="$TOLERANCE" '
        { d = $1 - $2; if (d > 2048) d -= 4096; if (d < -2048) d += 4096; if (d < 0) d = -d
          if (d > t) { n++; if (d > worst) worst = d } }
        END { print n + 0, worst + 0 }')"
    set -- $off
    [ "$1" -eq 0 ] \
        || fail "the scenery was drawn at a $field the camera never asked for on $1 frame(s), worst $2 units off"
done

pass
