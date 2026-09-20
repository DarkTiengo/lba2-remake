#!/usr/bin/env bash
# A mouse held still has not let go; a mouse that stops moving mid-drag must stop the camera.
#
# The orbit's release tail is the stick's behaviour: a stick springs back to centre, so "no input
# this frame" means the hand is off it, and easing the last speed out over a few frames is what
# keeps the orbit from halting dead. A mouse reports exactly the same thing while the button is
# still down and the hand is simply holding position — so the camera used to coast away from
# where the pointer stopped (#514).
#
# Both halves are asserted, because a build that simply deleted the tail would pass the first:
# holding still stops the camera, and letting go still eases out.
#
# Driven through the `mouse` verb rather than `camnudge`, which is the shortcut past the device
# layer this is about.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_mouse_release
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

DRAG=50   # a step this size only happens while the drag is driving
STOPPED=6 # what counts as "the camera is not moving" (the hero's own drift is a couple of units)
COASTING=20

held="$(mktemp)"; released="$(mktemp)"
trap 'rm -f "$held" "$released"' EXIT

# Same drag in both runs. The first keeps the button down and the pointer still afterwards; the
# second lets both go, which is a flick.
cam_run "$held" "" \
    --exec-at 20 "mouse 40 0 20 2" \
    --exec-at 40 "mouse 0 0 40 2" \
    --tick 90
cam_run "$released" "" \
    --exec-at 20 "mouse 40 0 20 2" \
    --tick 90

# The two frames after the drag's last driving frame: coasting shows up there or not at all.
after_drag() { # <log> -> the steps of the two frames following the drag
    cam_col "$1" step | awk -v d="$DRAG" '
        { s = $1 < 0 ? -$1 : $1; step[NR] = s; if (s >= d) last = NR }
        END { if (last) print step[last + 1] + 0, step[last + 2] + 0 }'
}

read -r h1 h2 < <(after_drag "$held")
[ -n "$h1" ] || fail "the drag never drove the camera: the fixture is not testing anything"
[ "$h1" -le "$STOPPED" ] && [ "$h2" -le "$STOPPED" ] \
    || fail "the camera kept moving ($h1, $h2 units) while the mouse was held still: the release tail is being spent under a live drag"

read -r r1 r2 < <(after_drag "$released")
[ -n "$r1" ] || fail "the second run's drag never drove the camera"
[ "$r1" -ge "$COASTING" ] \
    || fail "letting go stopped the orbit dead ($r1 units): the release tail is gone"

pass
