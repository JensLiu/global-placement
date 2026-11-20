# Render OpenROAD's own views of a bench placement directory, and dump the
# heatmaps as numbers. Run offscreen:
#   QT_QPA_PLATFORM=offscreen openroad -gui -no_init -exit render.tcl
#
#   BENCH_OUT_DIR  directory holding global_placement.odb and legal.odb
#                  (written by bench.tcl); images go to BENCH_OUT_DIR/images,
#                  heatmap CSVs to BENCH_OUT_DIR/heatmaps
#   BENCH_STAGE    global_placement | legal (one stage per OpenROAD process)
#   BENCH_IMAGE_WIDTH  pixels (default 900)
#
# Every placement uses the same fixed colour ranges, so images are directly
# comparable side by side:
#   Placement density 0-100 % (100 % = full; above saturates)
#   RUDY (estimated routing congestion) 0-120 %

set out $::env(BENCH_OUT_DIR)
set width [expr {[info exists ::env(BENCH_IMAGE_WIDTH)] ? $::env(BENCH_IMAGE_WIDTH) : 900}]
file mkdir $out/images $out/heatmaps

proc show_only { controls } {
  gui::set_display_controls "*" visible false
  foreach control $controls { gui::set_display_controls $control visible true }
}

proc heatmap { name control min max } {
  gui::set_heatmap $name DisplayMin $min
  gui::set_heatmap $name DisplayMax $max
  gui::set_heatmap $name ShowLegend 1
  gui::set_heatmap $name rebuild
  return $control
}

set stage $::env(BENCH_STAGE)
read_db $out/$stage.odb
# Frame the die explicitly: by default the view includes off-die cells, which
# would shrink the picture and stop panels of different placements lining up.
set die [[ord::get_db_block] getDieArea]
set dbu [[ord::get_db_block] getDbUnitsPerMicron]
set area [list [expr {[$die xMin] / double($dbu)}] [expr {[$die yMin] / double($dbu)}] \
  [expr {[$die xMax] / double($dbu)}] [expr {[$die yMax] / double($dbu)}]]
show_only {"Instances/*"}
save_image -area $area -width $width $out/images/${stage}_cells.png

set control [heatmap Placement "Heat Maps/Placement Density" 0 100]
show_only [list "Instances/*" $control]
save_image -area $area -width $width $out/images/${stage}_density.png
gui::dump_heatmap Placement $out/heatmaps/${stage}_density.csv

set control [heatmap RUDY "Heat Maps/Estimated Congestion (RUDY)" 0 120]
show_only [list "Instances/*" $control]
save_image -area $area -width $width $out/images/${stage}_rudy.png
gui::dump_heatmap RUDY $out/heatmaps/${stage}_rudy.csv
puts "RENDER wrote $out/images"
