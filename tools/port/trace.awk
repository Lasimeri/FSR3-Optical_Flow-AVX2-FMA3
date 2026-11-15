# trace.awk: --trace, a line for every picture that reached the glass.
$0 == "    a->ring[c & 255] = (struct shown){it.t_src, it.kind == IT_MID};" { print "    a->ring[c & 255] = (struct shown){.t_src = it.t_src, .generated = it.kind == IT_MID};"; a++; next }
$0 == "    int generated;" { print; print "    int hold, waiting;"; print "    uint64_t t_draw;"; a++; next }
$0 == "    int probe, pass_only, fatal;" { print "    int probe, pass_only, fatal, trace;"; a++; next }
$0 == "    a->cur_t = 0;" { print; print "    struct shown *const sh = &a->ring[c & 255];"; print "    sh->hold = a->cur_hold;"; print "    sh->waiting = (int)q_len(a);"; print "    sh->t_draw = a->t_swap;"; a++; next }
$0 == "        a->cur_t = t;" { print
    print "    if (a->trace)"
    print "        printf(\"  shown %llu %s, to hold %d, %d waiting behind it; drawn %.2f ms before its scanout (%s), %.2f refreshes after the one before\\n\", (unsigned long long)c, e->generated ? \"generated\" : \"real\", e->hold, e->waiting, (double)(t - e->t_draw) * 1e-6, a->deep ? \"deep\" : \"shallow\","
    print "               a->last_t && refresh ? (double)(t - a->last_t) / (double)refresh : 0.0);"; a++; next }
index($0, "        else if (!strcmp(argv[i], \"--no-excl\"))") == 1 { print "        else if (!strcmp(argv[i], \"--trace\"))"; print "            a.trace = 1;"; a++ }
{ print }
END { if (a != 6) { print "trace.awk: " a " anchors of 6" > "/dev/stderr"; exit 1 } }
