// ============================================================
// orrery_gear_train.scad
//
// 5 stepper stations, each: small pinion (on motor shaft) driving
// a large thin INTERNAL ring gear. Ring gears are concentric on
// the base (like orbit rings). Motors mount underneath the base,
// shaft pokes up through a hole, pinion sits on top and meshes
// with its ring gear from inside.
//
// REQUIRES: BOSL2 library -- https://github.com/BelfrySCAD/BOSL2
//   Download/clone it into your OpenSCAD "libraries" folder
//   (File > Show Library Folder in the OpenSCAD app), so you have:
//   <libraries>/BOSL2/std.scad, gears.scad, etc.
//
// WORKFLOW:
//   1. Tune STATIONS[] and SHAFT_* below to your actual parts.
//   2. Preview (F5) the whole assembly to check clearances.
//   3. Scroll to the RENDER SWITCH at the bottom, pick ONE part,
//      F6 + export STL, repeat for each part you need to print.
// ============================================================

include <BOSL2/std.scad>
include <BOSL2/gears.scad>

// ---------- Shared gear sizing (keep constant across all meshing pairs) ----------
circ_pitch      = 3.3;   // tooth size -- shrunk further to buy back room for the sun post
pressure_angle  = 20;
backlash        = 0.25;   // mm at pitch circle -- bumped up from 0.3, which had zero real margin for FDM
clearance       = undef; // let BOSL2 use its default (module/4)

pinion_thickness = 5;    // mm
ring_thickness    = 7;   // mm, thin as requested -- keep >= 3mm for FDM strength at this scale
ring_backing       = 3;    // mm of solid rim material outside the ring gear teeth (trimmed down to save space)

// ---------- Motor shaft: stubby double-flat ("double-D") stub ----------
// MEASURE YOUR ACTUAL MOTOR with calipers and edit these three numbers.
// shaft_dia    = diameter of the round part of the stub
// flat_span    = distance measured STRAIGHT ACROSS the two flat faces
//                (not the diameter -- the flats cut it down)
// shaft_len    = how far the stub sticks out (engagement depth for the pinion bore)
shaft_dia  = 5.0;
flat_span  = 3.5;
shaft_len  = 8;
bore_clearance = 0.45;   // mm added to bore so it slips on -- bumped up from 0.25

// ---------- Station layout ----------
// One entry per stepper: [ring_teeth, pinion_teeth, motor_angle_deg]
// motor_angle_deg = angular position around the base where THAT motor sits.
// Rings are concentric (same center), so ring radius grows automatically
// with ring_teeth.
//
// Mercury's ring_teeth jumped from 20 to 44 -- the old value put its own
// pinion's radius bigger than its distance from center, leaving negative
// room for a sun post. This is now checked at compile time too (see the
// sun-clearance assertion below).
//
// Sized for a 220mm Ender 3 bed: outermost ring works out to ~207mm
// diameter, leaving ~6mm margin per side. That's tighter than before --
// budget for brim/first-layer adhesion, don't treat it as guaranteed safe.
STATIONS = [
    [44,  10,   0],
    [83,  10,  90],
    [122, 10,  180],
    [161, 10, 270],
];

// ============================================================
// Derived geometry helpers
// ============================================================
function ring_pr(teeth) = pitch_radius(circ_pitch, teeth);
function ring_outer_r(teeth) = ring_pr(teeth) + module_value(circ_pitch) + ring_backing;
function pinion_outer_r(teeth) =
    outer_radius(circ_pitch=circ_pitch, teeth=teeth,
        profile_shift=auto_profile_shift(teeth=teeth, pressure_angle=pressure_angle));
function motor_center_dist(ring_teeth, pinion_teeth) =
    let(
        ps_pinion = auto_profile_shift(teeth = pinion_teeth, pressure_angle = pressure_angle),
        ps_ring   = max(ps_pinion, 0)
    )
    gear_dist(circ_pitch=circ_pitch, teeth1=ring_teeth, teeth2=pinion_teeth, internal1=true,
              profile_shift1=ps_ring, profile_shift2=ps_pinion);

// ---------- Planet post ----------
// Sits in the SOLID BACKING RIM of the ring gear (outside the teeth), not
// on the teeth themselves. EDIT these three to change the peg size/position.
ADD_PLANET_POST    = false; // set false to remove the peg entirely (0-height was causing a compile issue)
planet_post_dia    = 4;   // diameter of the peg the planet mounts onto
planet_post_height = 8;   // how far it sticks up above the ring gear's top face
planet_post_angle  = 0;   // where around the ring it sits -- doesn't matter mechanically
                           // (the ring spins freely) but lets you keep them visually aligned
function planet_post_r(teeth) = ring_outer_r(teeth) - ring_backing / 2; // middle of the backing band

// ---------- Support (idler) gear ----------
// A second, non-driven gear meshing with the same ring, 180 deg opposite
// the drive pinion. Holds the ring centered/supported without a channel.
// It spins freely on a fixed post (a plain peg -- print it into the base,
// or press a metal pin/screw into it) -- NOT on a motor shaft.
ADD_SUPPORT_GEAR       = true;
support_post_dia        = 5;    // diameter of the fixed peg the idler spins on
support_bore_clearance  = 0.9;  // extra clearance so it spins freely -- loosened further from 0.6
support_post_height     = pinion_thickness + 2; // base post height -- must support full idler thickness

// ---------- Sanity check: does each outer station's pinion clear the next ring inward? ----------
// This is the exact collision you saw with Venus punching through Mercury --
// now it's caught at compile time instead of in the preview.
MIN_STATION_GAP = 3; // mm, minimum radial clearance required between stations
for (i = [1 : len(STATIONS) - 1]) {
    inner = STATIONS[i - 1];
    outer = STATIONS[i];
    inner_outer_r = ring_outer_r(inner[0]);
    outer_pinion_inner_edge = motor_center_dist(outer[0], outer[1]) - pinion_outer_r(outer[1]);
    gap = outer_pinion_inner_edge - inner_outer_r;
    assert(gap > MIN_STATION_GAP,
        str("Station ", i, " (ring_teeth=", outer[0], ") collides with station ", i - 1,
            " (ring_teeth=", inner[0], "): only ", gap,
            "mm radial gap. Increase this station's ring_teeth so its pinion clears the inner ring."));
}

// ---------- Sun post ----------
// A fixed, non-rotating peg at dead center for the sun. Its clearance is
// set by the INNERMOST station's pinion -- same collision math as between
// two rings, just checked against the center instead of a neighbor ring.
sun_post_dia    = 18; // mm
sun_post_height = 15; // mm
SUN_CLEARANCE_MARGIN = 2; // mm, extra margin beyond the bare post radius

sun_clear_r = motor_center_dist(STATIONS[0][0], STATIONS[0][1]) - pinion_outer_r(STATIONS[0][1]);
assert(sun_clear_r > sun_post_dia / 2 + SUN_CLEARANCE_MARGIN,
    str("Innermost ring's pinion leaves only ", sun_clear_r,
        "mm clearance at center -- need at least ", sun_post_dia / 2 + SUN_CLEARANCE_MARGIN,
        "mm for the sun post. Increase station 0's ring_teeth, or shrink sun_post_dia."));

module sun_post() {
    cylinder(d = sun_post_dia, h = sun_post_height, $fn = 32);
}

// ============================================================
// Parts
// ============================================================

module double_flat_bore(h, z_center = 0) {
    // Round hole with two parallel flats cut symmetrically -- for the
    // "stubby, hard edges, 2 flat sides" motor shaft.
    dia = shaft_dia + bore_clearance;
    flat = flat_span + bore_clearance;
    cut_offset = flat / 2;
    translate([0, 0, z_center])
    difference() {
        cylinder(d = dia, h = h, center = true, $fn = 48);
        translate([0, cut_offset + 10, 0]) cube([dia + 1, 20, h + 1], center = true);
        translate([0, -cut_offset - 10, 0]) cube([dia + 1, 20, h + 1], center = true);
    }
}

module pinion(ring_teeth, pinion_teeth) {
    ps = auto_profile_shift(teeth = pinion_teeth, pressure_angle = pressure_angle);
    difference() {
        spur_gear(
            circ_pitch     = circ_pitch,
            teeth          = pinion_teeth,
            thickness      = pinion_thickness,
            pressure_angle = pressure_angle,
            profile_shift  = ps,
            backlash       = backlash,
            clearance      = clearance,
            anchor         = BOTTOM
        );
        double_flat_bore(shaft_len + 2, z_center = pinion_thickness / 2);
    }
}

// Non-driven idler: same tooth geometry as the pinion, but a plain round
// bore (no double-flat) since it just spins freely on a fixed peg.
module idler_gear(ring_teeth, pinion_teeth) {
    ps = auto_profile_shift(teeth = pinion_teeth, pressure_angle = pressure_angle);
    difference() {
        spur_gear(
            circ_pitch     = circ_pitch,
            teeth          = pinion_teeth,
            thickness      = pinion_thickness,
            pressure_angle = pressure_angle,
            profile_shift  = ps,
            backlash       = backlash,
            clearance      = clearance,
            anchor         = BOTTOM
        );
        translate([0, 0, -1])
            cylinder(d = support_post_dia + support_bore_clearance, h = pinion_thickness + 2, $fn = 32);
    }
}

// The base is already printed -- its outer wall position was computed from
// ring_backing directly, with almost no margin (only 0.4mm total). We can't
// change the base now, so instead we shrink the PRINTED ring gear's body by
// this extra amount, so it comes out smaller than what the fixed wall
// assumed and actually has real clearance to sit inside it.
RING_FIT_CLEARANCE = 1.0; // mm shaved off the ring's outer backing, print-side only -- loosened further from 0.5

// ---------- Weld flange ----------
// A wider flat platform sitting ABOVE the (already-printed, fixed) wall's
// top edge -- the thin planet posts alone are too weak to carry real
// weight. This gives real surface area to weld a proper support arm to,
// starting right where the fixed wall ends so it never collides with it.
// The planet post still pokes up through it as a small locator pin.
ADD_WELD_FLANGE    = true;
FLANGE_THICKNESS    = 3;  // mm, thickness of the flat platform itself
FLANGE_EXTRA_WIDTH  = 6; // mm, how far it extends beyond the wall's outer face

module weld_flange(ring_teeth) {
    inner_r        = ring_pr(ring_teeth) - module_value(circ_pitch) * 1.5;
    wall_outer_r   = ring_outer_r(ring_teeth) + 0.4 + 2;
    flange_outer_r = wall_outer_r + FLANGE_EXTRA_WIDTH;
    riser_outer_r  = ring_outer_r(ring_teeth) - RING_FIT_CLEARANCE;
    flange_z       = max(channel_wall_h, ring_thickness); // never start below the ring's own top
    union() {
        if (flange_z > ring_thickness)
            translate([0, 0, ring_thickness])
                tube(or = riser_outer_r, ir = inner_r, h = flange_z - ring_thickness, $fn = 150);
        translate([0, 0, flange_z])
            tube(or = flange_outer_r, ir = inner_r, h = FLANGE_THICKNESS, $fn = 150);
    }
}

module ring_gear_thin(ring_teeth, pinion_teeth) {
    ps_pinion = auto_profile_shift(teeth = pinion_teeth, pressure_angle = pressure_angle);
    union() {
        ring_gear(
            circ_pitch     = circ_pitch,
            teeth          = ring_teeth,
            thickness      = ring_thickness,
            backing        = ring_backing - RING_FIT_CLEARANCE,
            pressure_angle = pressure_angle,
            profile_shift  = max(ps_pinion, 0), // ring profile shift must be >= mating pinion's
            backlash       = backlash,
            clearance      = clearance,
            anchor         = BOTTOM
        );
        // planet post, sitting on top of the solid backing rim
        if (ADD_PLANET_POST)
            rotate([0, 0, planet_post_angle])
                translate([planet_post_r(ring_teeth), 0, ring_thickness])
                    cylinder(d = planet_post_dia, h = planet_post_height, $fn = 24);
        if (ADD_WELD_FLANGE)
            weld_flange(ring_teeth);
    }
}

// A single station preview: ring gear + its meshing pinion (+ idler), positioned correctly
module station_preview(ring_teeth, pinion_teeth, motor_angle) {
    color("orange") ring_gear_thin(ring_teeth, pinion_teeth);
    d = motor_center_dist(ring_teeth, pinion_teeth);
    color("steelblue")
        rotate([0, 0, motor_angle])
        translate([d, 0, 0])
        pinion(ring_teeth, pinion_teeth);
    if (ADD_SUPPORT_GEAR)
        color("mediumseagreen")
            rotate([0, 0, motor_angle + 180])
            translate([d, 0, 0])
            idler_gear(ring_teeth, pinion_teeth);
}

module all_stations_preview() {
    for (s = STATIONS)
        station_preview(s[0], s[1], s[2]);
}

// ---------- Base plate ----------
// A flat disc, an OUTER retaining wall per ring (the inner side is handled
// by the support/idler gear, not a wall), support posts for the idlers,
// and motor shaft + screw holes at each drive pinion position. No lid.
base_thickness  = 4;
channel_wall_h  = ring_thickness - 1; // + 1.5; // taller than the gear so it actually retains it
// Real 28BYJ-48 mounting geometry (measured, common across manufacturers):
motor_screw_spacing = 35;   // distance between the two mount hole centers
motor_screw_dia      = 4.2; // mount hole diameter on the motor's own bracket
motor_shaft_offset   = 8;   // the mount hole LINE sits offset this far from the shaft -- NOT centered on it
motor_mount_angle    = 40;   // degrees -- rotates the two screw holes AROUND the shaft axis,
                             // independent of where the shaft sits on the base. Use this to
                             // clock the motor body (e.g. route wires away from the next ring).

module base_plate() {
    max_r = max([for (s = STATIONS) ring_outer_r(s[0])]) + 15;
    difference() {
        union() {
            cylinder(r = max_r, h = base_thickness, $fn = 150);
            // outer wall only, per ring -- fully punched through top-to-bottom (no capped ceiling)
            for (s = STATIONS) {
                or_ = ring_outer_r(s[0]) + 0.4;
                translate([0, 0, base_thickness])
                difference() {
                    cylinder(r = or_ + 2, h = channel_wall_h, $fn = 150);
                    translate([0, 0, -1])
                        cylinder(r = or_, h = channel_wall_h + 2, $fn = 150);
                }
            }
            // support posts for the idler gears
            if (ADD_SUPPORT_GEAR)
                for (s = STATIONS) {
                    d = motor_center_dist(s[0], s[1]);
                    rotate([0, 0, s[2] + 180])
                    translate([d, 0, base_thickness])
                        cylinder(d = support_post_dia, h = support_post_height, $fn = 32);
                }
        }
        // motor shaft through-hole + screw holes, at the DRIVE pinion position for each station
        for (s = STATIONS) {
            d = motor_center_dist(s[0], s[1]);
            rotate([0, 0, s[2]])
            translate([d, 0, -1]) {
                cylinder(d = shaft_dia + 2, h = base_thickness + 2, $fn = 32); // shaft clearance
                // real 28BYJ-48: the two mount holes sit on a line offset
                // motor_shaft_offset (perpendicular) from the shaft -- NOT
                // through it. Getting this wrong is why they didn't line up.
                // motor_mount_angle spins this whole hole pair around the
                // shaft, independent of the station's own placement angle.
                rotate([0, 0, motor_mount_angle]) {
                    translate([motor_screw_spacing/2, -motor_shaft_offset, 0])
                        cylinder(d = motor_screw_dia, h = base_thickness + 2, $fn = 20);
                    translate([-motor_screw_spacing/2, -motor_shaft_offset, 0])
                        cylinder(d = motor_screw_dia, h = base_thickness + 2, $fn = 20);
                }
            }
        }
    }
}

// ---------- Test fitment template (full diameter, paper-thin) ----------
// The whole base's real footprint, sliced down to a couple mm thick --
// no walls, no full height, just every shaft hole and screw hole pair for
// EVERY station at their real positions and real spacing. Lay actual
// motors on it to check the whole layout at once. Prints fast because
// almost all the volume is gone, not because the diameter shrank.
TEST_TEMPLATE_THICKNESS = 2; // mm -- thin enough to print in minutes, thick enough to handle

module test_template() {
    intersection() {
        base_plate();
        cylinder(r = 1000, h = TEST_TEMPLATE_THICKNESS, $fn = 100); // keeps only a thin bottom slice
    }
}

// ---------- Test fitment wedge ----------
// A pie-slice cut straight out of the REAL base_plate() geometry -- not a
// simplified stand-in -- so if this fits your motor and a ring gear, the
// full base will too. Prints in minutes instead of hours.
// Includes: the shaft hole + screw holes for one station, plus a chunk of
// that station's outer retaining wall (to test-fit a ring gear against).
TEST_WEDGE_STATION   = 0;    // which station to test -- 0 is smallest/fastest
TEST_WEDGE_HALF_ANGLE = 30;  // degrees each side of the motor -- widen if holes get clipped
TEST_WEDGE_MAX_R       = undef; // auto = that station's outer wall + 10mm if left undef

module test_wedge() {
    s = STATIONS[TEST_WEDGE_STATION];
    r = (TEST_WEDGE_MAX_R == undef) ? ring_outer_r(s[0]) + 10 : TEST_WEDGE_MAX_R;
    intersection() {
        base_plate();
        rotate([0, 0, s[2]]) // aim the wedge at this station's motor angle
        linear_extrude(height = base_thickness + channel_wall_h + 5)
            polygon(concat(
                [[0, 0]],
                [for (a = [-TEST_WEDGE_HALF_ANGLE : 2 : TEST_WEDGE_HALF_ANGLE]) [r * cos(a), r * sin(a)]]
            ));
    }
}

// ---------- Leg (separate part -- weld to the base with a 3D pen) ----------
// Motors sitting under the base only prop up the side where they're
// clustered (~120 deg of the perimeter with 5 stations at 0/30/60/90/120).
// The rest of the base has nothing holding it level. These are standalone
// feet you print separately and glue/weld to the underside wherever the
// base needs support -- MEASURE how far your motor body actually hangs
// below the base and set leg_height to match, or the base will tilt.
leg_height  = 19;  // mm -- default assumes a 28BYJ-48-style motor body; measure yours
leg_dia     = 12;  // mm -- shaft of the leg
foot_dia    = 18;  // mm -- flat pad at the bottom, gives more surface area to weld/glue
foot_h      = 3;   // mm -- pad thickness

module leg() {
    union() {
        cylinder(d = foot_dia, h = foot_h, $fn = 32); // flat foot pad on the floor
        translate([0, 0, foot_h])
            cylinder(d = leg_dia, h = leg_height - foot_h, $fn = 32); // weld this top face to the base
    }
}

// ============================================================
// RENDER SWITCH -- uncomment ONE section at a time before F6 export
// ============================================================

// PART selects what gets rendered. Set it here for GUI use (F5/F6), or
// override from the command line with -D 'PART="pinion"' -D PART_INDEX=0
// for scripted batch export (see the accompanying export script).
PART       = "preview"; // "preview" | "pinion" | "idler" | "ring" | "base" | "template" | "test_wedge" | "leg" | "sun"
PART_INDEX = 0;         // which station (0-based) for pinion/idler/ring

if (PART == "preview") {
    translate([0, 0, base_thickness]) all_stations_preview(); // sits on top of the flat plate
    translate([0, 0, base_thickness]) color("gold") sun_post();
    color("dimgray") base_plate();
} else if (PART == "pinion") {
    pinion(STATIONS[PART_INDEX][0], STATIONS[PART_INDEX][1]);
} else if (PART == "idler") {
    idler_gear(STATIONS[PART_INDEX][0], STATIONS[PART_INDEX][1]);
} else if (PART == "ring") {
    ring_gear_thin(STATIONS[PART_INDEX][0], STATIONS[PART_INDEX][1]);
} else if (PART == "base") {
    base_plate();
} else if (PART == "template") {
    test_template();
} else if (PART == "test_wedge") {
    test_wedge();
} else if (PART == "leg") {
    leg();
} else if (PART == "sun") {
    sun_post();
}
