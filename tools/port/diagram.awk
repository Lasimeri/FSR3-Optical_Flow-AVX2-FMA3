# diagram.awk: a box-drawing diagram from a spec, every line padded to one width and then counted: a line of another width fails the run.
function rep(s, n,   r) { r = ""; while (n-- > 0) r = r s; return r }
function out(s) { if (length(s) != W) { printf "width %d, not %d: %s\n", length(s), W, s > "/dev/stderr"; bad = 1 } print s; n++ }
function row(s) { out("║ " s rep(" ", W - 4 - length(s)) " ║") }
BEGIN { FS = "|"; W = 98; I = W - 4 }
$1 == "T" { out("╔══ " $2 " " rep("═", W - 6 - length($2)) "╗"); row("") }
$1 == "B" { row("┌─ " $2 " " rep("─", I - 5 - length($2)) "┐") }
$1 == "L" { row("│ " $2 rep(" ", I - 4 - length($2)) " │") }
$1 == "E" { row("└" rep("─", 8) "┬" rep("─", I - 11) "┘") }
$1 == "F" { row("└" rep("─", I - 2) "┘") }
$1 == "A" { row(rep(" ", 9) "▼  " $2) }
END { row(""); out("╚" rep("═", W - 2) "╝"); if (bad) exit 1; printf "%d lines of %d columns\n", n, W > "/dev/stderr" }
