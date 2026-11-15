# planes.awk: a cast whose dmabuf has more than one plane (AMD's compressed layouts carry their metadata in further planes): every plane goes to EGL, and the buffers are not asked to be single-block.
$0 == "static struct pwbuf *import(struct app *a, const struct spa_data *d)" {
    print "/* A cast buffer as a texture. Every plane of the dmabuf is named to EGL: a modifier with metadata planes (AMD's DCC) has up to four. */"
    print "static struct pwbuf *import(struct app *a, const struct spa_buffer *buf)"; getline; print
    print "    static const EGLint P_FD[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};"
    print "    static const EGLint P_OFF[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};"
    print "    static const EGLint P_PITCH[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};"
    print "    static const EGLint P_LO[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};"
    print "    static const EGLint P_HI[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};"
    print "    const struct spa_data *d = &buf->datas[0];"; a++; next }
$0 == "    EGLAttrib at[24];" { print "    EGLAttrib at[64];"; a++; next }
$0 == "    AT(EGL_DMA_BUF_PLANE0_FD_EXT, d->fd);" {
    print "    for (uint32_t pl = 0; pl < buf->n_datas && pl < 4; pl++) {"
    print "        const struct spa_data *e = &buf->datas[pl];"
    print "        if (e->type != SPA_DATA_DmaBuf || !e->chunk)"
    print "            break;"
    print "        AT(P_FD[pl], e->fd);"
    print "        AT(P_OFF[pl], e->chunk->offset);"
    print "        AT(P_PITCH[pl], e->chunk->stride ? e->chunk->stride : a->W * 4);"
    print "        if (a->raw.modifier != MOD_INVALID) {"
    print "            AT(P_LO[pl], a->raw.modifier & 0xffffffffu);"
    print "            AT(P_HI[pl], a->raw.modifier >> 32);"
    print "        }"
    print "    }"; skip = 6; a++; next }
skip > 0 { skip--; next }
index($0, "the cast's dmabuf (fd %d, pitch %d, offset %u, modifier %#llx) could not be imported: EGL error %#x\\n\", (int)d->fd,") { sub(/the cast's dmabuf \(fd %d,/, "the cast's dmabuf (%u planes; the first: fd %d,"); sub(/\(int\)d->fd,/, "buf->n_datas, (int)d->fd,"); a++ }
$0 == "        pb = import(a, d);" { print "        pb = import(a, b->buffer);"; a++; next }
index($0, "SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), ") { sub(/SPA_PARAM_BUFFERS_blocks, SPA_POD_Int\(1\), /, ""); a++ }
{ print }
END { if (a != 6) { print "planes.awk: " a " anchors of 6" > "/dev/stderr"; exit 1 } }
