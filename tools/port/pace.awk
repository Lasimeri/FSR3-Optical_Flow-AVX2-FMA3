# pace (this file keeps its name in the pipeline): the frame callback's instant is kept, and the refresh the compositor reports replaces the mode's.
$0 == "static void on_frame_callback(void *u) { ((struct app *)u)->can_draw = 1; }" {
    print "/* The compositor is ready for the next picture: the start of a cycle of its own, the phase every later picture is timed from. */"
    print "static void on_frame_callback(void *u)"
    print "{"
    print "    struct app *a = u;"
    print "    a->can_draw = 1;"
    print "    a->t_fc = of_now_ns();"
    print "}"; a++; next }
$0 == "    if (c + 1 == a->commits)" {
    print "    if (refresh) {"
    print "        a->refresh_ns = refresh;"
    print "        /* How deep the compositor is just now: from this picture's own way to the glass. */"
    print "        if (e->t_draw && t > e->t_draw)"
    print "            a->deep = t - e->t_draw > (uint64_t)refresh + refresh / 6;"
    print "    }"; a++ }
$0 == "    int rc = 0, excluded = 0;" {
    print "    printf(\"holds: %s\\n\", a.rate ? \"a picture's refreshes are counted from the frame callback while KWin is deep, from its own scanout while shallow\" : \"none: one picture a refresh, by the frame callback\");"
    a++ }
{ print }
END { if (a != 3) { print "pace: " a " anchors of 3" > "/dev/stderr"; exit 1 } }
