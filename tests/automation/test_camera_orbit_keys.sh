#!/usr/bin/env bash
# The camera orbits on the keyboard, through a binding the player can change.
#
# Orbiting used to read the bracket scancodes straight out of GereExtKeys: the keys worked but
# were in no menu, in no key list, and could not be moved -- and on a keyboard that is not a US
# one they are not where their legends say. They are input slots now (INPUT_SLOT_CAM_LEFT /
# _RIGHT), bound to Q and E with the brackets kept as the second binding, so they show up in
# Options > Configure keyboard and travel through lba2.cfg like every other action.
#
# Asserted: all four default keys orbit, the two directions go opposite ways, and a slot rebound
# in the cfg follows the new key -- which is the half that proves these are bindings and not two
# more hardcoded scancodes.
#
# Local-only (needs retail data + the tracked corpus save). Not in host_quick CI.
TESTNAME=camera_orbit_keys
. "$(dirname "$0")/lib.sh"
. "$(dirname "$0")/camlib.sh"
cam_precheck

SAVE="$REPO/tests/savegame/corpus/saves/steam_classic_2023/Desert Island.LBA"
[ -f "$SAVE" ] || skip "fixture missing: $SAVE"

TURN=300 # units of beta a 90-frame hold must be worth; the hold is 30 a frame

log="$(mktemp)"; dir="$(mktemp -d)"
trap 'rm -rf "$log" "$dir"' EXIT

# How far beta travelled over the run, signed, the short way round.
turned() { # <scancode> [user-dir]
    ctl_headless --load "$SAVE" --fixed-dt 16 ${2:+--user-dir "$2"} \
        --exec "cam_follow 1; cam_hold_angle 1; camtrace 1" \
        --exec-at 30 "key $1 90" \
        --tick 160 --exit > "$log" 2>&1 || fail "engine run failed: exit $?"
    cam_col "$log" beta | awk '
        NR == 1 { first = $1 }
        { last = $1 }
        END { d = last - first; if (d > 2048) d -= 4096; if (d < -2048) d += 4096; print d }'
}

# SDL scancodes: Q 20, E 8, [ 47, ] 48, T 23.
for key in 20 47; do
    d="$(turned $key)"
    [ "$d" -lt "-$TURN" ] || fail "scancode $key turned the camera $d units: the left orbit is not bound to it"
done
for key in 8 48; do
    d="$(turned $key)"
    [ "$d" -gt "$TURN" ] || fail "scancode $key turned the camera $d units: the right orbit is not bound to it"
done

# Rebound in the cfg, the way the remap screen writes it: the new key turns and the old one does
# not. A cfg is needed first, so the engine writes one and this edits the slot in it.
ctl_headless --user-dir "$dir" --tick 30 --exit > /dev/null 2>&1 || fail "could not write a cfg"
[ -f "$dir/lba2.cfg" ] || fail "no lba2.cfg written: the bindings have nowhere to live"
grep -q "^Input36_1: 20" "$dir/lba2.cfg" \
    || fail "the cfg does not carry the camera-orbit slot at its default: $(grep '^Input36' "$dir/lba2.cfg" || echo none)"
sed -i 's/^Input36_1: .*/Input36_1: 23/' "$dir/lba2.cfg"

d="$(turned 23 "$dir")"
[ "$d" -lt "-$TURN" ] || fail "the rebound key turned the camera $d units: the cfg binding was not honoured"

pass
