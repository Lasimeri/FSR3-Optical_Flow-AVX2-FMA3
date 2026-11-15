# ofgl.awk: mvexgl.c (the card's link) into ofgl.c (the in-process engine, rates and holds). Every edit is anchored on one exact line; a missing anchor fails the run.
function emit(file,   l) { while ((getline l < file) > 0) print l; close(file) }
function hit() { anchors++ }
BEGIN { emit(top) }
NR <= 9 { next }
tail { next }
$0 == "static void print_second(struct app *a, double secs)" { emit(mainf); tail = 1; hit(); next }
{
    gsub(/MF_SLOTS/, "OF_SLOTS"); gsub(/MF_RING/, "OF_RING"); gsub(/mf_now_ns/, "of_now_ns"); gsub(/MF_T_TOTAL/, "OF_T_TOTAL"); gsub(/mvexgl/, "ofgl")
}
$0 == "#include \"mf_link.h\"" { print "#include \"engine.h\""; hit(); next }
$0 == "struct item {" { in_item = 1 }
in_item && $0 == "    uint64_t t_src;" { print; print "    int hold; /* refreshes it stays on the glass */"; in_item = 0; hit(); next }
$0 == "    int mid_done, late;" { print "    int mid_done, late, real_hold;"; hit(); next }
$0 == "    uint64_t card_sum, card_n, card_max, moved_sum;" { print "    uint64_t card_sum, card_n, card_max, moved_sum, wait_sum;"; hit(); next }
$0 == "    struct mf_link l;" { print "    struct of_engine eng;"; hit(); next }
$0 == "    struct mf_layout lay;" { hit(); next }
$0 == "    int W, H, mhz, div, lw, lh, lstride, bw, bh;" { print "    int W, H, mhz, div, lw, lh, lstride, bw, bh, rate;"; hit(); next }
$0 == "    uint64_t t_swap;" { print "    uint64_t t_swap, t_fc, cur_t, refresh_ns, pair_num, pair_den, pair_acc;"; print "    int cur_hold, deep;"; hit(); next }
$0 == "static void enqueue_real(struct app *a, int frame, uint64_t t_src)" { print "static void enqueue_real(struct app *a, int frame, uint64_t t_src, int hold)"; hit(); next }
$0 == "    a->q[a->q_tail++ % Q_N] = (struct item){IT_REAL, -1, frame, -1, t_src};" { print "    a->q[a->q_tail++ % Q_N] = (struct item){IT_REAL, -1, frame, -1, t_src, hold};"; hit(); next }
$0 ~ /^ +enqueue_real\(a, s, pts\);$/ { sub(/pts\);/, "pts, 1);"); print; hit(); next }
$0 == "            enqueue_real(a, j->in, j->pts);" { n_jr++; print (n_jr == 1 ? "            enqueue_real(a, j->in, j->pts, j->real_hold ? j->real_hold : 1);" : "            enqueue_real(a, j->in, j->pts, 1);"); hit(); next }
$0 == "    struct mf_msg *m = &a->l.ctl->msg[seq % OF_RING];" {
    print "    a->eng.msg[seq % OF_RING] = (struct of_msg){.seq = seq, .in_slot = (uint32_t)s, .out_slot = (uint32_t)(want_mid ? o : 0), .flags = want_mid ? 0u : (uint32_t)OF_NO_MID};"
    print "    of_engine_submit(&a->eng, seq);"; skip = 6; hit(); next }
skip > 0 { skip--; next }
$0 == "    const uint64_t have = a->l.ctl->mid_pub;" { print "    const uint64_t have = __atomic_load_n(&a->eng.gen_pub, __ATOMIC_SEQ_CST);"; hit(); next }
$0 == "        if (j->mid_done)" { print "        /* A frame whose answer is no field (a scene change) is take_done's. */"; print "        if (j->mid_done || a->eng.gen[s % OF_RING].made != 1)"; hit(); next }
index($0, "a->l.part + mf_out_offset(&a->lay, (uint32_t)j->out)") { sub(/a->l\.part \+ mf_out_offset\(&a->lay, \(uint32_t\)j->out\)/, "a->eng.vec[j->out]"); print; hit(); next }
$0 == "            a->q[a->q_tail++ % Q_N] = (struct item){IT_MID, j->a, j->in, j->out, j->pts - a->interval_ns / 2};" {
    print "            /* What this source frame is worth in refreshes, the fraction carried: the generated picture takes the smaller half, the real frame after it the rest. */"
    print "            a->pair_acc += a->pair_num;"
    print "            const int pair = (int)(a->pair_acc / a->pair_den);"
    print "            a->pair_acc %= a->pair_den;"
    print "            const int g_hold = pair / 2 > 0 ? pair / 2 : 1;"
    print "            j->real_hold = pair - g_hold > 0 ? pair - g_hold : 1;"
    print "            a->q[a->q_tail++ % Q_N] = (struct item){IT_MID, j->a, j->in, j->out, j->pts - a->interval_ns / 2, g_hold};"; hit(); next }
$0 == "    const uint64_t have = a->l.ctl->gen_pub;" { print "    const uint64_t have = __atomic_load_n(&a->eng.gen_pub, __ATOMIC_SEQ_CST);"; hit(); next }
$0 == "        const struct mf_gen g = a->l.ctl->gen[seq % OF_RING];" { print "        const struct of_gen g = a->eng.gen[seq % OF_RING];"; hit(); next }
$0 == "        a->s.card_n++;" { print; print "        a->s.wait_sum += g.us[OF_T_WAIT];"; hit(); next }
$0 == "/* One picture for the refresh the compositor has just asked for. */" {
    print "/* When the next picture may be drawn so that the one on the glass gets its refreshes, and is not replaced before it was ever shown. 0: at once. KWin has two depths and moves between them, and each needs its own clock:"
    print " *   deep (a commit reaches the glass 1 to 2 refreshes later: KWin composes right after a vblank): the frame callback marks the start of KWin's cycle; that instant plus whole refreshes is the same point of a later"
    print " *     cycle. A picture held h refreshes is followed h cycles on; one held 1 is followed at the frame callback itself."
    print " *   shallow (under 1 refresh: KWin composes just before the vblank): the frame callback can come while the picture still waits for its cycle, and the next one sent then replaces it unseen (measured: generated"
    print " *     pictures discarded). There the clock is the picture's own scanout: the next one goes just after the vblank that ends the held refreshes, far from where KWin latches."
    print " * Measured and rejected: the scanout clock in the deep state (1 and 3 refreshes for 2 and 2), and wp_fifo barriers in either (a barrier falls at a refresh its commit missed). docs/measurements.md. */"
    print "static uint64_t hold_due(const struct app *a)"
    print "{"
    print "    if (a->deep)"
    print "        return a->cur_hold > 1 && a->t_fc ? a->t_fc + (uint64_t)(a->cur_hold - 1) * a->refresh_ns : 0;"
    print "    if (a->cur_t)"
    print "        return a->cur_t + (uint64_t)(a->cur_hold - 1) * a->refresh_ns + 300000ull;"
    print "    /* Not shown yet, or its feedback never came: a refresh past the time it must have been taken up. */"
    print "    return a->t_swap ? a->t_swap + (uint64_t)a->cur_hold * a->refresh_ns + 1000000ull : 0;"
    print "}"
    print ""
    print "/* One picture for a refresh the compositor has asked for, once the picture before it has had its refreshes. */"; hit(); next }
$0 == "    if (!a->can_draw || a->q_head == a->q_tail)" { print; getline; print; print "    const uint64_t due = hold_due(a);"; print "    if (due && now < due)"; print "        return;"; hit(); next }
$0 == "    a->t_swap = of_now_ns();" { print; print "    /* Behind by more than a source frame (a generated picture and its real frame may always wait): one refresh less, until the queue is short again. */"; print "    a->cur_hold = q_len(a) >= 3 && it.hold > 1 ? it.hold - 1 : it.hold;"; print "    a->cur_t = 0;"; hit(); next }
$0 == "    const struct shown *e = &a->ring[c & 255];" { print; print "    if (c + 1 == a->commits)"; print "        a->cur_t = t;"; hit(); next }
index($0, "a->probe ? a->probe_luma : a->l.part + mf_in_offset(&a->lay, (uint32_t)s)") { sub(/a->l\.part \+ mf_in_offset\(&a->lay, \(uint32_t\)s\)/, "a->eng.luma[s]"); print; hit(); next }
{ print }
END { if (anchors != want) { print "ofgl.awk: " anchors " anchors of " want > "/dev/stderr"; exit 1 } }
