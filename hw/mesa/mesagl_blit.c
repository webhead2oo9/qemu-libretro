/*
 * QEMU MESA GL Pass-Through
 *
 *  Copyright (c) ... in a Galaxy far, far away ...
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library;
 * if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "mesagl_impl.h"

int mesa_gui_fullscreen(const void *);

void MesaContextAttest(const char *div, int *out)
{
    const char *aiv[] = { ATTEST_IV };
    *out = 1;
    for (int i = 0; aiv[i]; i++) {
        *out = memcmp(div, aiv[i], strlen(aiv[i]))? 0:1;
        if (*out) break;
    }
}

static struct {
    unsigned vao, vbo;
    int prog, vert, frag, black;
    int adj, flip, has_swap;
} blit;
static unsigned blit_program_setup(void)
{
    MESA_PFN(PFNGLATTACHSHADERPROC,       glAttachShader);
    MESA_PFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
    MESA_PFN(PFNGLCOMPILESHADERPROC,      glCompileShader);
    MESA_PFN(PFNGLCREATEPROGRAMPROC,      glCreateProgram);
    MESA_PFN(PFNGLCREATESHADERPROC,       glCreateShader);
    MESA_PFN(PFNGLGETINTEGERVPROC,        glGetIntegerv);
    MESA_PFN(PFNGLGETSTRINGPROC,          glGetString);
    MESA_PFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    MESA_PFN(PFNGLLINKPROGRAMPROC,        glLinkProgram);
    MESA_PFN(PFNGLSHADERSOURCEPROC,       glShaderSource);
    MESA_PFN(PFNGLUSEPROGRAMPROC,         glUseProgram);
    const char *vert_src[] = {
        "#version 120\n"
        "attribute vec2 in_position;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  texcoord = vec2(1 + in_position.x, 1 + in_position.y) * 0.5;\n"
        "  gl_Position = vec4(in_position, 0, 1);\n"
        "}\n",
        "#version 140\n"
        "#extension GL_ARB_explicit_attrib_location : require\n"
        "layout (location = 0) in vec2 in_position;\n"
        "out vec2 texcoord;\n"
        "void main() {\n"
        "  texcoord = vec2(1 + in_position.x, 1 + in_position.y) * 0.5;\n"
        "  gl_Position = vec4(in_position, 0, 1);\n"
        "}\n"
    };
    const char *frag_src[] = {
        "#version 120\n"
        "uniform sampler2D screen_texture;\n"
        "uniform bool frag_just_black;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  if (frag_just_black)\n"
        "    gl_FragColor = vec4(0,0,0,1);\n"
        "  else\n"
        "    gl_FragColor = texture2D(screen_texture, texcoord);\n"
        "}\n",
        "#version 140\n"
        "uniform sampler2D screen_texture;\n"
        "uniform bool frag_just_black;\n"
        "in vec2 texcoord;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  if (frag_just_black)\n"
        "    fragColor = vec4(0,0,0,1);\n"
        "  else\n"
        "    fragColor = texture(screen_texture, texcoord);\n"
        "}\n"
    };
    int prog;
    if (!blit.prog) {
        int i = memcmp(PFN_CALL(glGetString(GL_VERSION)), "2.1 Metal",
                sizeof("2.1 Metal") - 1)? 1:0,
            srclen = ALIGNED((strlen(vert_src[i])+1));
        char *srcbuf = g_new0(char, srclen);
        const char *vert_buf[] = { srcbuf };
        strncpy(srcbuf, vert_src[i], srclen);
        if (blit.flip) {
            char *flip = strstr(srcbuf, "+ in_position.y");
            *flip = '-';
        }
        blit.vert = PFN_CALL(glCreateShader(GL_VERTEX_SHADER));
        PFN_CALL(glShaderSource(blit.vert, 1, vert_buf, 0));
        PFN_CALL(glCompileShader(blit.vert));
        g_free(srcbuf);
        blit.frag = PFN_CALL(glCreateShader(GL_FRAGMENT_SHADER));
        PFN_CALL(glShaderSource(blit.frag, 1, &frag_src[i], 0));
        PFN_CALL(glCompileShader(blit.frag));
        prog = PFN_CALL(glCreateProgram());
        PFN_CALL(glAttachShader(prog, blit.vert));
        PFN_CALL(glAttachShader(prog, blit.frag));
        if (!i)
            PFN_CALL(glBindAttribLocation(prog, 0, "in_position"));
        PFN_CALL(glLinkProgram(prog));
        blit.prog = prog;
    }
    PFN_CALL(glGetIntegerv(GL_CURRENT_PROGRAM, &prog));
    PFN_CALL(glUseProgram(blit.prog));
    blit.black = PFN_CALL(glGetUniformLocation(blit.prog, "frag_just_black"));
    return prog;
}
void MesaBlitFree(void)
{
    MESA_PFN(PFNGLDELETEBUFFERSPROC,      glDeleteBuffers);
    MESA_PFN(PFNGLDELETEPROGRAMPROC,      glDeleteProgram);
    MESA_PFN(PFNGLDELETESHADERPROC,       glDeleteShader);
    MESA_PFN(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
    if (blit.prog) {
        PFN_CALL(glDeleteProgram(blit.prog));
        PFN_CALL(glDeleteShader(blit.vert));
        PFN_CALL(glDeleteShader(blit.frag));
    }
    if (blit.vbo)
        PFN_CALL(glDeleteBuffers(1, &blit.vbo));
    if (blit.vao)
        PFN_CALL(glDeleteVertexArrays(1, &blit.vao));
    memset(&blit, 0, sizeof(blit));
}
struct save_states {
    int view[4];
    int draw_binding, read_binding, texture, texture_binding,
        vao_binding, vbo_binding, boolean_map;
};
#define FRAMEBUFFER_SRGB_(s) \
    (s.boolean_map & 2)
struct states_mapping {
    int gl_enum, *iv;
};
static const int boolean_states[] = {
    GL_FRAMEBUFFER_SRGB,
    GL_BLEND,
    GL_CULL_FACE,
    GL_DEPTH_TEST,
    GL_SCISSOR_TEST,
    GL_STENCIL_TEST,
    0,
};
static int blit_program_buffer(void *save_map, const int size, const void *data)
{
    MESA_PFN(PFNGLBINDBUFFERPROC,      glBindBuffer);
    MESA_PFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    MESA_PFN(PFNGLBUFFERDATAPROC,      glBufferData);
    MESA_PFN(PFNGLDISABLEPROC,         glDisable);
    MESA_PFN(PFNGLGENBUFFERSPROC,      glGenBuffers);
    MESA_PFN(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    MESA_PFN(PFNGLGETINTEGERVPROC,     glGetIntegerv);
    MESA_PFN(PFNGLISENABLEDPROC,       glIsEnabled);

    struct save_states *last = (struct save_states *)save_map;

    struct states_mapping mapping[] = {
        { GL_VIEWPORT, last->view },
        { GL_FRAMEBUFFER_BINDING, &last->draw_binding },
        { GL_READ_FRAMEBUFFER_BINDING, &last->read_binding },
        { GL_ACTIVE_TEXTURE, &last->texture },
        { GL_TEXTURE_BINDING_2D, &last->texture_binding },
        { GL_VERTEX_ARRAY_BINDING, &last->vao_binding },
        { GL_ARRAY_BUFFER_BINDING, &last->vbo_binding },
        { GL_CONTEXT_PROFILE_MASK, &last->boolean_map },
        { 0, 0 },
    };
    for (int i = 0; mapping[i].gl_enum; i++)
        PFN_CALL(glGetIntegerv(mapping[i].gl_enum, mapping[i].iv));
    last->boolean_map &= GL_CONTEXT_CORE_PROFILE_BIT;

    for (int i = 0; boolean_states[i]; i++) {
        last->boolean_map |= PFN_CALL(glIsEnabled(boolean_states[i]))? (2 << i):0;
        if (last->boolean_map & (2 << i))
            PFN_CALL(glDisable(boolean_states[i]));
    }
    if (last->boolean_map & GL_CONTEXT_CORE_PROFILE_BIT) {
        if (!blit.vao)
            PFN_CALL(glGenVertexArrays(1, &blit.vao));
        PFN_CALL(glBindVertexArray(blit.vao));
    }
    if (!blit.vbo)
        PFN_CALL(glGenBuffers(1, &blit.vbo));
    PFN_CALL(glBindBuffer(GL_ARRAY_BUFFER, blit.vbo));
    PFN_CALL(glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW));
    return 0;
}
static void blit_restore_savemap(const void *save_map)
{
    MESA_PFN(PFNGLBINDBUFFERPROC,               glBindBuffer);
    MESA_PFN(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray);
    MESA_PFN(PFNGLENABLEPROC,                   glEnable);

    struct save_states *last = (struct save_states *)save_map;

    if (last->boolean_map & GL_CONTEXT_CORE_PROFILE_BIT)
        PFN_CALL(glBindVertexArray(last->vao_binding));

    PFN_CALL(glBindBuffer(GL_ARRAY_BUFFER, last->vbo_binding));

    for (int i = 0; boolean_states[i]; i++) {
        if ((boolean_states[i] == GL_FRAMEBUFFER_SRGB)
                && !(last->read_binding == last->draw_binding))
            continue;
        if (last->boolean_map & (2 << i))
            PFN_CALL(glEnable(boolean_states[i]));
    }
}
int MesaBlitScale(void)
{
    int scaled = 0;
    MESA_PFN(PFNGLACTIVETEXTUREPROC,            glActiveTexture);
    MESA_PFN(PFNGLBINDTEXTUREPROC,              glBindTexture);
    MESA_PFN(PFNGLBLITFRAMEBUFFERPROC,          glBlitFramebuffer);
    MESA_PFN(PFNGLCOPYTEXIMAGE2DPROC,           glCopyTexImage2D);
    MESA_PFN(PFNGLDELETETEXTURESPROC,           glDeleteTextures);
    MESA_PFN(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray);
    MESA_PFN(PFNGLDRAWARRAYSPROC,               glDrawArrays);
    MESA_PFN(PFNGLENABLEPROC,                   glEnable);
    MESA_PFN(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray);
    MESA_PFN(PFNGLGENTEXTURESPROC,              glGenTextures);
    MESA_PFN(PFNGLTEXPARAMETERIPROC,            glTexParameteri);
    MESA_PFN(PFNGLUNIFORM1IPROC,                glUniform1i);
    MESA_PFN(PFNGLUSEPROGRAMPROC,               glUseProgram);
    MESA_PFN(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer);
    MESA_PFN(PFNGLVIEWPORTPROC,                 glViewport);

    int v[4], fullscreen = mesa_gui_fullscreen(v);
    blit.has_swap = 1;

    if (blit.adj) {
        blit.adj = !blit.adj;
        return scaled;
    }
    blit.flip = ScalerBlitFlip();

    if (DrawableContext()
            && (v[3] > (v[1] & 0x7FFFU))
            && (!fullscreen || RenderScalerOff())) {
        unsigned screen_texture, w = v[0], h = v[1] & 0x7FFFU,
                last_prog = blit_program_setup();
        int aspect = (v[1] & (1 << 15))? 0:1,
                offs_x = v[2] - ((v[0] * 1.f * v[3]) / (v[1] & 0x7FFFU));
        offs_x >>= 1;
        v[0] *= (1.f * v[3]) / (v[1] & 0x7FFFU);
        v[1] = v[3];
        const float coord[] = {
            1-((1.f * v[2] - v[0]) / v[2]),-1,  1,-1,
            1-((1.f * v[2] - v[0]) / v[2]), 1,  1, 1,
            -1,-1, ((1.f * v[2] - v[0]) / v[2])-1,-1,
            -1, 1, ((1.f * v[2] - v[0]) / v[2])-1, 1,
            -1,-1,  1,-1,  -1,1,  1,1,
        };

        struct save_states save_map;

        if (!blit_program_buffer(&save_map, sizeof(coord), coord)) {
            PFN_CALL(glUniform1i(blit.black, GL_TRUE));
            PFN_CALL(glViewport(0,0,  v[2], v[3]));
            PFN_CALL(glEnableVertexAttribArray(0));
            PFN_CALL(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0));
            if (save_map.read_binding == save_map.draw_binding) {
                PFN_CALL(glActiveTexture(GL_TEXTURE0));
                PFN_CALL(glGenTextures(1, &screen_texture));
                PFN_CALL(glBindTexture(GL_TEXTURE_2D, screen_texture));
                PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
                PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
                PFN_CALL(glCopyTexImage2D(GL_TEXTURE_2D, 0, (FRAMEBUFFER_SRGB_(save_map) && ScalerSRGBCorr())?
                            GL_SRGB:GL_RGBA, 0,0, w,h, 0));
                if (aspect) {
                    PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)); /* clear */
                    PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 4, 4)); /* clear */
                    PFN_CALL(glViewport(offs_x,0,  v[0],v[1]));
                }
                PFN_CALL(glUniform1i(blit.black, GL_FALSE));
                PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 8, 4)); /* scale */
                PFN_CALL(glDeleteTextures(1, &screen_texture));
                PFN_CALL(glActiveTexture(save_map.texture));
                PFN_CALL(glBindTexture(GL_TEXTURE_2D, save_map.texture_binding));
            }
            else {
                if (FRAMEBUFFER_SRGB_(save_map))
                    PFN_CALL(glEnable(boolean_states[0]));
                if (aspect) {
                    PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)); /* clear */
                    PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 4, 4)); /* clear */
                    PFN_CALL(glBlitFramebuffer(0,0,w,h, offs_x,v[1],v[0]+offs_x,0,
                        (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT),
                        GL_NEAREST));
                }
                else
                    PFN_CALL(glBlitFramebuffer(0,0,w,h, 0,v[3],v[2],0,
                        (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT),
                        GL_NEAREST));
            }
            PFN_CALL(glDisableVertexAttribArray(0));
            PFN_CALL(glViewport(save_map.view[0], save_map.view[1],
                                save_map.view[2], save_map.view[3]));
            blit_restore_savemap(&save_map);
            scaled = 1;
        }
        PFN_CALL(glUseProgram(last_prog));
    }
    /* qemu-libretro: tell the caller whether this present pass ran (it
     * applies blit.flip when it does), so the readback glue knows if a
     * guest-requested flip is still pending in the back buffer. */
    return scaled;
}

void MesaRenderScaler(const uint32_t FEnum, void *args)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);
    int v[4], fullscreen = mesa_gui_fullscreen(v), framebuffer_binding, blit_adj = 0;
    uint32_t *box;

    PFN_CALL(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer_binding));

    switch(FEnum) {
        case FEnum_glBlitFramebuffer:
        case FEnum_glBlitFramebufferEXT:
            box = &((uint32_t *)args)[4];
            blit_adj = 1;
            break;
        case FEnum_glScissor:
        case FEnum_glViewport:
            box = args;
            break;
        case GL_VIEWPORT:
            box = args;
            if (!box[0] && !box[1] && (v[3] > (v[1] & 0x7FFFU))) {
                box[2] = v[0];
                box[3] = v[1] & 0x7FFFU;
            }
            /* fall through */
        default:
            return;
    }
    if (DrawableContext() && !framebuffer_binding
            && (v[3] > (v[1] & 0x7FFFU))
            && (fullscreen || !blit.has_swap)
            && !RenderScalerOff()) {
        int aspect = (v[1] & (1 << 15))? 0:1,
            offs_x = v[2] - ((v[0] * 1.f * v[3]) / (v[1] & 0x7FFFU));
        offs_x >>= 1;
        for (int i = 0; i < 4; i++)
            box[i] *= (1.f * v[3]) / (v[1] & 0x7FFFU);
        if (aspect) {
            box[0] += offs_x;
            box[2] += (blit_adj)? box[0]:0;
        }
        else {
            box[0] *= (1.f * v[2]) / box[2];
            box[2] = v[2];
        }
        blit.adj = blit_adj;
    }
}

/* XPD-DIAG helper: read a framebuffer's color attachment and write it as a
 * bottom-up 24-bit BMP so it can be viewed directly. Widths chosen (800, 512)
 * keep rows 4-byte aligned, so no per-row padding is needed. */
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif
static void MesaSurfaceCopyDumpBmp(int fb, int w, int h, const char *path)
{
    MESA_PFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    MESA_PFN(PFNGLREADPIXELSPROC,      glReadPixels);
    unsigned char *pix;
    unsigned char fh[14], ih[40];
    int row = w * 3;
    int imgsize = row * h;
    int filesize = 54 + imgsize;
    FILE *fp;

    pix = (unsigned char *)malloc(imgsize);
    if (!pix)
        return;
    PFN_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb));
    PFN_CALL(glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pix));

    memset(fh, 0, sizeof(fh));
    memset(ih, 0, sizeof(ih));
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = filesize & 0xff; fh[3] = (filesize >> 8) & 0xff;
    fh[4] = (filesize >> 16) & 0xff; fh[5] = (filesize >> 24) & 0xff;
    fh[10] = 54;
    ih[0] = 40;
    ih[4] = w & 0xff; ih[5] = (w >> 8) & 0xff;
    ih[6] = (w >> 16) & 0xff; ih[7] = (w >> 24) & 0xff;
    ih[8] = h & 0xff; ih[9] = (h >> 8) & 0xff;
    ih[10] = (h >> 16) & 0xff; ih[11] = (h >> 24) & 0xff;
    ih[12] = 1;
    ih[14] = 24;
    ih[20] = imgsize & 0xff; ih[21] = (imgsize >> 8) & 0xff;
    ih[22] = (imgsize >> 16) & 0xff; ih[23] = (imgsize >> 24) & 0xff;

    fp = fopen(path, "wb");
    if (fp) {
        fwrite(fh, 1, 14, fp);
        fwrite(ih, 1, 40, fp);
        fwrite(pix, 1, imgsize, fp);
        fclose(fp);
    }
    free(pix);
}

/* XPD-DIAG (bounded, remove after root cause): ordered timeline of the title
 * frames around the accelerated DirectDraw surface copy. Tracing arms on the
 * first FBO->FBO copy whose source holds a rendered frame (non-black center),
 * then logs every COPY / DRAW / PRESENT with target FBO, blend, texture and
 * viewport state until three presents complete. Dumps the copy source, copy
 * destination and first presented frame as BMPs. */
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST 0x0BC0
#endif
#define XPD_TRACE_LOG \
    "C:\\Users\\charlie\\dev\\XPDriver\\qemu\\winxp\\host-frame-trace.log"

static int xpd_trace_active;
static int xpd_trace_lines;
static int xpd_trace_presents;

/* Texture name attached to COLOR_ATTACHMENT0 of the currently bound DRAW
 * framebuffer (0 when the default framebuffer or nothing is bound). */
static int xpd_draw_attachment(void)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);
    MESA_PFN(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC,
             glGetFramebufferAttachmentParameteriv);
    int draw_fb = 0, att = 0;

    PFN_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb));
    if (!draw_fb)
        return 0;
    PFN_CALL(glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att));
    return att;
}

/* Reports the current READ framebuffer binding and its color attachment so
 * the ReadPixels dump probe can attribute each readback to its source. */
void MesaReadbackOriginProbe(int *read_fb, int *attachment)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);
    MESA_PFN(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC,
             glGetFramebufferAttachmentParameteriv);
    int fb = 0, att = 0;

    PFN_CALL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fb));
    if (fb)
        PFN_CALL(glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
            &att));
    *read_fb = fb;
    *attachment = att;
}

/* FBO plumbing calls logged while the trace is active so the timeline shows
 * every render-target change (attach + bind), not just draws.
 * kind: 0=FramebufferTexture2D 1=BindFramebuffer 2=DeleteTextures 3=Clear */
void MesaFboEventProbe(int fenum_kind, const uint32_t *arg)
{
    FILE *fp;

    if (!xpd_trace_active || xpd_trace_lines >= 120000)
        return;
    xpd_trace_lines++;
    fp = fopen(XPD_TRACE_LOG, "a");
    if (!fp)
        return;
    if (fenum_kind == 0)
        fprintf(fp, "FBTEX target=%04x att=%04x textarget=%04x name=%u\n",
                arg[0], arg[1], arg[2], arg[3]);
    else if (fenum_kind == 1)
        fprintf(fp, "BINDFB target=%04x name=%u\n", arg[0], arg[1]);
    else if (fenum_kind == 2)
        fprintf(fp, "DELTEX n=%u\n", arg[0]);
    else
        fprintf(fp, "CLEAR mask=%04x rt=%d\n", arg[0],
                xpd_draw_attachment());
    fclose(fp);
}

static int xpd_kill_alphatest(void)
{
    static int checked, kill;

    if (!checked) {
        const char *v = getenv("XPD_KILL_ALPHATEST");
        kill = v && v[0] == '1';
        checked = 1;
    }
    return kill;
}

/* Post-exec undo for glEnable(GL_ALPHA_TEST) while the kill switch is on. */
void MesaAlphaKillProbe(unsigned int cap)
{
    MESA_PFN(PFNGLDISABLEPROC, glDisable);

    if (cap == GL_ALPHA_TEST && xpd_kill_alphatest())
        PFN_CALL(glDisable(GL_ALPHA_TEST));
}

void MesaDrawStateProbe(int mode, int count)
{
    MESA_PFN(PFNGLGETINTEGERVPROC,   glGetIntegerv);
    MESA_PFN(PFNGLGETFLOATVPROC,     glGetFloatv);
    MESA_PFN(PFNGLISENABLEDPROC,     glIsEnabled);
    MESA_PFN(PFNGLGETBOOLEANVPROC,   glGetBooleanv);
    MESA_PFN(PFNGLACTIVETEXTUREPROC, glActiveTexture);
    MESA_PFN(PFNGLDISABLEPROC,       glDisable);
    int draw_fb = 0, vp[4] = {0};
    int bsrc = 0, bdst = 0, bsrca = 0, bdsta = 0, atex = 0, tex = 0;
    int prog = 0, tex0 = 0, tex1 = 0, afunc = 0;
    float aref = 0.0f;
    unsigned char blend, ztest, zmask = 1, atest, sciss;
    FILE *fp;

    if (!xpd_trace_active || xpd_trace_lines >= 120000)
        return;
    xpd_trace_lines++;

    PFN_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb));
    PFN_CALL(glGetIntegerv(GL_VIEWPORT, vp));
    blend = PFN_CALL(glIsEnabled(GL_BLEND));
    ztest = PFN_CALL(glIsEnabled(GL_DEPTH_TEST));
    atest = PFN_CALL(glIsEnabled(GL_ALPHA_TEST));
    sciss = PFN_CALL(glIsEnabled(GL_SCISSOR_TEST));
    PFN_CALL(glGetBooleanv(GL_DEPTH_WRITEMASK, &zmask));
    PFN_CALL(glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc));
    PFN_CALL(glGetIntegerv(GL_BLEND_DST_RGB, &bdst));
    PFN_CALL(glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca));
    PFN_CALL(glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta));
    PFN_CALL(glGetIntegerv(GL_ACTIVE_TEXTURE, &atex));
    PFN_CALL(glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex));
    PFN_CALL(glGetIntegerv(GL_CURRENT_PROGRAM, &prog));
    PFN_CALL(glGetIntegerv(GL_ALPHA_TEST_FUNC, &afunc));
    PFN_CALL(glGetFloatv(GL_ALPHA_TEST_REF, &aref));
    PFN_CALL(glActiveTexture(GL_TEXTURE0));
    PFN_CALL(glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0));
    PFN_CALL(glActiveTexture(GL_TEXTURE0 + 1));
    PFN_CALL(glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1));
    PFN_CALL(glActiveTexture(atex));

    /* Kill switch: suppress alpha test from the next draw onward (the
     * matching glEnable undo lives in MesaAlphaKillProbe). */
    if (atest && xpd_kill_alphatest())
        PFN_CALL(glDisable(GL_ALPHA_TEST));

    fp = fopen(XPD_TRACE_LOG, "a");
    if (fp) {
        fprintf(fp,
            "DRAW fbo=%d rt=%d mode=%d n=%d vp=%d,%d,%dx%d blend=%d "
            "rgb=%04x/%04x a=%04x/%04x ztest=%d zw=%d at=%d sc=%d "
            "unit=%d tex=%d prog=%d t0=%d t1=%d af=%04x ar=%.2f\n",
            draw_fb, xpd_draw_attachment(), mode, count,
            vp[0], vp[1], vp[2], vp[3], blend,
            bsrc, bdst, bsrca, bdsta, ztest, zmask, atest, sciss,
            atex - 0x84C0, tex, prog, tex0, tex1, afunc, aref);
        fclose(fp);
    }
}

void MesaSurfaceCopyProbe(const uint32_t *arg)
{
    MESA_PFN(PFNGLGETINTEGERVPROC,     glGetIntegerv);
    MESA_PFN(PFNGLREADPIXELSPROC,      glReadPixels);
    MESA_PFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    MESA_PFN(PFNGLBINDBUFFERPROC,      glBindBuffer);
    MESA_PFN(PFNGLPIXELSTOREIPROC,     glPixelStorei);
    static int present_dumped = 0;
    int read_fb = 0, draw_fb = 0, pack_buf = 0, pack_align = 4;
    unsigned char s2[4] = {0};
    FILE *fp;

    PFN_CALL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb));
    PFN_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb));

    if (read_fb && draw_fb && read_fb != draw_fb) {
        /* accelerated surface copy (FBO -> FBO) */
        PFN_CALL(glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf));
        PFN_CALL(glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align));
        if (pack_buf)
            PFN_CALL(glBindBuffer(GL_PIXEL_PACK_BUFFER, 0));
        PFN_CALL(glPixelStorei(GL_PACK_ALIGNMENT, 1));

        PFN_CALL(glReadPixels(400, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, s2));
        if (!xpd_trace_active && !xpd_trace_presents &&
            (s2[0] || s2[1] || s2[2])) {
            xpd_trace_active = 1;
            MesaSurfaceCopyDumpBmp(read_fb, 800, 600,
                "C:\\Users\\charlie\\dev\\XPDriver\\qemu\\winxp\\host-blit-source.bmp");
            MesaSurfaceCopyDumpBmp(draw_fb, 512, 512,
                "C:\\Users\\charlie\\dev\\XPDriver\\qemu\\winxp\\host-blit-dest.bmp");
            PFN_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb));
        }

        PFN_CALL(glPixelStorei(GL_PACK_ALIGNMENT, pack_align));
        if (pack_buf)
            PFN_CALL(glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buf));

        if (xpd_trace_active) {
            fp = fopen(XPD_TRACE_LOG, "a");
            if (fp) {
                fprintf(fp,
                    "COPY read_fb=%d draw_fb=%d dst_rt=%d src=(%u,%u,%u,%u) "
                    "dst=(%u,%u,%u,%u) center=%u,%u,%u\n",
                    read_fb, draw_fb, xpd_draw_attachment(),
                    arg[0], arg[1], arg[2], arg[3],
                    arg[4], arg[5], arg[6], arg[7], s2[0], s2[1], s2[2]);
                fclose(fp);
            }
        }
    } else if (read_fb && !draw_fb && xpd_trace_active) {
        /* present blit (render target -> window framebuffer 0) */
        xpd_trace_presents++;
        fp = fopen(XPD_TRACE_LOG, "a");
        if (fp) {
            fprintf(fp,
                "PRESENT #%d read_fb=%d src=(%u,%u,%u,%u) dst=(%u,%u,%u,%u)\n",
                xpd_trace_presents, read_fb, arg[0], arg[1], arg[2], arg[3],
                arg[4], arg[5], arg[6], arg[7]);
            fclose(fp);
        }
        /* Rolling sample of what each present hands to framebuffer 0:
         * every 8th present overwrites a 4-slot ring, so the final ring
         * holds the era the user last saw. */
        if ((xpd_trace_presents & 7) == 1) {
            char ppath[128];
            snprintf(ppath, sizeof(ppath),
                "C:\\Users\\charlie\\dev\\XPDriver\\qemu\\winxp\\"
                "host-present-ring-%d.bmp", (xpd_trace_presents >> 3) & 3);
            MesaSurfaceCopyDumpBmp(read_fb, 800, 600, ppath);
        }
        (void)present_dumped;
    }
}

