#!/usr/bin/env bash
# The island's cloud ceiling is a hard limit for the eye, and it is eased into.
#
# The ceiling (Sky_Y) is a textured plane the engine draws over the island. The software painter
# always draws it behind the world, so the classic camera could rise through it and never show
# it; the GPU renderer gives it depth, so an eye above it sees a floor of cloud with the island
# hidden underneath. Tilting up is enough to get there: at the elevation limit the boom puts the
# eye thousands of units over the ceiling.
#
# Two things are asserted, because either alone passes for the wrong reason: the eye stays under
# the ceiling (the constraint), and it gets there a step at a time (the constraint is a camera
# move, not a teleport, and a snap would read as a cut on screen).
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=followcam_ceiling
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

CLEARANCE=200 # FOLLOW_CAM_CEILING_CLEARANCE_DEFAULT
TOLERANCE=40  # FOLLOW_CAM_GROUND_SNAP: the ease snaps to target within this

log="$(mktemp)"; loose="$(mktemp)"
trap 'rm -f "$log" "$loose"' EXIT

# Tilt to the elevation limit and hold it there. The corpus save is on Zeelich, whose ceiling is
# low, so the boom crosses it well before the tilt runs out.
cam_run "$log" "" \
    --exec-at 20 "camnudge 0 40 40" \
    --tick 160

# The run has to reach the ceiling at all, or the rest asserts nothing.
crossed="$(paste <(cam_col "$log" eye) <(cam_col "$log" sky) |
    awk -v c="$CLEARANCE" '$2 - c < $1 + 3000 { n++ } END { print n + 0 }')"
[ "$crossed" -gt 0 ] \
    || fail "the tilt never brought the eye near the ceiling: the fixture is not testing anything"

over="$(paste <(cam_col "$log" eye) <(cam_col "$log" sky) |
    awk -v c="$CLEARANCE" -v t="$TOLERANCE" '$1 > $2 - c + t { n++ } END { print n + 0 }')"
[ "$over" -eq 0 ] \
    || fail "the eye rose above the cloud ceiling on $over frame(s): the island would be hidden behind it"

# Eased, not snapped. The lift's own ease divides the remaining distance by 8 with a floor, so a
# frame never moves more than an eighth of the gap; a jump of thousands is the failure this
# catches (an earlier always-on snap on the ground path had to be reverted for exactly that).
jump="$(cam_col "$log" eye | awk '
    NR > 1 { d = $1 - prev; if (d < 0) d = -d; if (d > max) max = d }
    { prev = $1 }
    END { print max + 0 }')"
[ "$jump" -lt 3000 ] \
    || fail "the eye moved $jump units in one frame: the ceiling is snapping rather than easing"

# And it is the ceiling doing it: with the constraint off, the same tilt goes straight through.
cam_run "$loose" "cam_ceiling 0" \
    --exec-at 20 "camnudge 0 40 40" \
    --tick 160
through="$(paste <(cam_col "$loose" eye) <(cam_col "$loose" sky) |
    awk '$1 > $2 { n++ } END { print n + 0 }')"
[ "$through" -gt 0 ] \
    || fail "cam_ceiling 0 kept the eye under the ceiling too: this fixture would pass without the feature"

pass
