# Placement benchmark: (optionally) global-place, then legalise with OpenROAD's
# detailed_placement only, measure, then apply DPO + mirroring as a secondary
# measurement. Driven by scripts/bench.py; all inputs come from the environment.
#
#   BENCH_IN        input ODB (3_2 for references, our written placement otherwise)
#   BENCH_OUT_DIR   existing output directory (bench.json, ODBs)
#   BENCH_MODE      gpl | legalise
#   BENCH_DENSITY   GPL target density, or "auto" (uniform density + 0.01)   [gpl]
#   BENCH_ROUTABILITY  1 to add -routability_driven                          [gpl]
#   BENCH_FASTROUTE_TCL, BENCH_ROUTING_LAYERS  global-routing setup (optional)
#
# HPWL here is the same definition as the tracer's metrics.jsonl and matches
# OpenROAD's GPL/DPL log values: ITerm getAvgXY (bbox centre if absent), BPin
# bbox centres, all non-supply nets.

set in $::env(BENCH_IN)
set out $::env(BENCH_OUT_DIR)
set mode $::env(BENCH_MODE)
read_db $in
set block [ord::get_db_block]
set dbu [$block getDbUnitsPerMicron]

proc env_or { name default } {
  if { [info exists ::env($name)] && $::env($name) ne "" } { return $::env($name) }
  return $default
}

proc bench_hpwl { block dbu {csv ""} } {
  set total 0.0
  if { $csv ne "" } {
    set fh [open $csv w]
    puts $fh "net,pins,hpwl_um"
  }
  foreach net [$block getNets] {
    set type [$net getSigType]
    if { $type eq "POWER" || $type eq "GROUND" } { continue }
    set xs {}
    set ys {}
    foreach it [$net getITerms] {
      set r [$it getAvgXY]
      if { [lindex $r 0] } {
        lappend xs [lindex $r 1]
        lappend ys [lindex $r 2]
      } else {
        set b [[$it getInst] getBBox]
        lappend xs [expr {([$b xMin] + [$b xMax]) / 2.0}]
        lappend ys [expr {([$b yMin] + [$b yMax]) / 2.0}]
      }
    }
    foreach bt [$net getBTerms] {
      foreach bp [$bt getBPins] {
        set b [$bp getBBox]
        lappend xs [expr {([$b xMin] + [$b xMax]) / 2.0}]
        lappend ys [expr {([$b yMin] + [$b yMax]) / 2.0}]
      }
    }
    if { [llength $xs] < 2 } { continue }
    set length [expr {([tcl::mathfunc::max {*}$xs] - [tcl::mathfunc::min {*}$xs]) \
                      + ([tcl::mathfunc::max {*}$ys] - [tcl::mathfunc::min {*}$ys])}]
    set total [expr {$total + $length}]
    if { $csv ne "" } {
      puts $fh "[string map {, ;} [$net getName]],[llength $xs],[expr {$length / $dbu}]"
    }
  }
  if { $csv ne "" } { close $fh }
  return [expr {$total / $dbu}]
}

# Movable = the registry's rule: everything except FIRM / LOCKED / COVER.
proc movable_centres { block } {
  set centres [dict create]
  foreach inst [$block getInsts] {
    set status [$inst getPlacementStatus]
    if { $status eq "FIRM" || $status eq "LOCKED" || $status eq "COVER" } { continue }
    set b [$inst getBBox]
    dict set centres [$inst getName] \
      [list [expr {([$b xMin] + [$b xMax]) / 2.0}] [expr {([$b yMin] + [$b yMax]) / 2.0}]]
  }
  return $centres
}

proc displacement { before after dbu } {
  set total 0.0
  set max 0.0
  set n 0
  dict for {name p} $before {
    set q [dict get $after $name]
    set d [expr {hypot([lindex $q 0] - [lindex $p 0], [lindex $q 1] - [lindex $p 1]) / $dbu}]
    set total [expr {$total + $d}]
    if { $d > $max } { set max $d }
    incr n
  }
  return [list $total [expr {$n ? $total / $n : 0.0}] $max $n]
}

proc json_string { s } {
  return "\"[string map {\\ \\\\ \" \\\" \n \\n} $s]\""
}

set result [dict create mode [json_string $mode] input [json_string $in]]

# Optional global-routing setup (needed by -routability_driven; later by GRT).
set fastroute [env_or BENCH_FASTROUTE_TCL ""]
if { $fastroute ne "" } { source $fastroute }
set layers [env_or BENCH_ROUTING_LAYERS ""]
if { $layers ne "" } { set_routing_layers -signal $layers }

if { $mode eq "gpl" } {
  set density [env_or BENCH_DENSITY auto]
  if { $density eq "auto" } {
    set density [expr {[gpl::get_global_placement_uniform_density] + 0.01}]
  }
  set args [list -density $density -force_center_initial_place]
  if { [env_or BENCH_ROUTABILITY 0] } { lappend args -routability_driven }
  set t0 [clock milliseconds]
  global_placement {*}$args
  dict set result gp_seconds [expr {([clock milliseconds] - $t0) / 1000.0}]
  dict set result density $density
  dict set result gp_args [json_string $args]
  write_db $out/global_placement.odb
}

dict set result gp_hpwl_um [bench_hpwl $block $dbu]
set before [movable_centres $block]
set t0 [clock milliseconds]
if { [catch { detailed_placement } err] } {
  dict set result legal false
  dict set result error [json_string $err]
} else {
  dict set result legalise_seconds [expr {([clock milliseconds] - $t0) / 1000.0}]
  set violations [catch { check_placement -verbose } check_err]
  dict set result legal [expr {$violations ? "false" : "true"}]
  if { $violations } { dict set result error [json_string $check_err] }
  set legal_hpwl [bench_hpwl $block $dbu $out/legal_nets.csv]
  dict set result legal_hpwl_um $legal_hpwl
  lassign [displacement $before [movable_centres $block] $dbu] total mean max count
  dict set result displacement_um \
    "{\"total\": $total, \"mean\": $mean, \"max\": $max, \"instances\": $count}"
  write_db $out/legal.odb
  # Secondary: ORFS's detailed-placement optimisation, applied equally to all.
  set t0 [clock milliseconds]
  if { [catch { improve_placement -max_displacement {5 1}; optimize_mirroring } err] } {
    dict set result dpo_error [json_string $err]
  } else {
    dict set result dpo_seconds [expr {([clock milliseconds] - $t0) / 1000.0}]
    dict set result dpo_hpwl_um [bench_hpwl $block $dbu]
    write_db $out/legal_dpo.odb
  }
}

set fields {}
dict for {key value} $result { lappend fields "[json_string $key]: $value" }
set fh [open $out/bench.json w]
puts $fh "\{[join $fields {, }]\}"
close $fh
puts "BENCH wrote $out/bench.json"
