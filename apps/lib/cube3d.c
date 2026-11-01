/* Implementation of the small 3D layer declared in cube3d.h.
 *
 * The pipeline, per frame:
 *
 *   1. rotate every vertex by the three view angles
 *   2. project the 8 rotated vertices to canvas space, once each
 *   3. for each face: compute its normal, light it, cull it if it points
 *      away from the camera, otherwise remember its average depth
 *   4. sort the surviving faces back-to-front and blit each as a quad
 *
 * Step 2 is what keeps this cheap: the projection runs eight times per
 * frame, not twenty-four, because the faces share vertices.
 *
 * There is no depth buffer.  A cube's faces never intersect, so painter's
 * algorithm plus back-face culling is both correct here and far cheaper than
 * a per-pixel Z test - and the affine texture mapper in apt would not
 * interpolate a Z value anyway.
 */
#include "cube3d.h"

/* ------------------------------------------------------------------ */
/* projection                                                          */

/* Perspective divide, returning a *canvas pixel* offset.
 *
 * Both arguments are 16.16, so dividing them directly yields a plain
 * integer - which is exactly what a pixel coordinate wants.  Shifting the
 * numerator up by 16 first (the obvious-looking "keep the fraction" move)
 * is wrong twice over: it overflows 32 bits for any numerator above 32768,
 * and it would leave the result scaled by 65536, so APT_FX_INT() on it
 * would return zero and collapse the whole cube to a point. */
static int project_div(apt_fx num, apt_fx den)
{
    if (den <= 0)
        return 0;
    return (int)((s32)num / (s32)den);
}

/* ------------------------------------------------------------------ */
/* textures                                                            */

/* Six distinct faces: a coloured border, a grid, and a big letter.  A plain
 * checkerboard looks identical when rotated, which is precisely the kind of
 * mis-mapping this demo exists to catch. */
static const u8 letter_glyph[6][7] = {
    { 0x1E, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },  /* F */
    { 0x1E, 0x12, 0x12, 0x1E, 0x14, 0x12, 0x11 },  /* R */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },  /* B */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1E },  /* L */
    { 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x04 },  /* U */
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }   /* D */
};

void cube_init(cube_mesh *m, int ts)
{
    static const u32 face_colour[6] = {
        APT_RGB(210, 70, 60),    /* front  - red    */
        APT_RGB(70, 190, 90),    /* right  - green  */
        APT_RGB(70, 120, 220),   /* back   - blue   */
        APT_RGB(220, 180, 60),   /* left   - amber  */
        APT_RGB(190, 90, 200),   /* up     - violet */
        APT_RGB(70, 200, 200)    /* down   - cyan   */
    };
    static const int fv[6][4] = {
        { 4, 5, 6, 7 },   /* front  (+z) */
        { 5, 1, 2, 6 },   /* right  (+x) */
        { 1, 0, 3, 2 },   /* back   (-z) */
        { 0, 4, 7, 3 },   /* left   (-x) */
        { 7, 6, 2, 3 },   /* up     (+y) */
        { 0, 1, 5, 4 }    /* down   (-y) */
    };
    int f, x, y, i;

    /* A cube of side 2, centred on the origin. */
    m->verts[0] = vec3_make(-1, -1, -1);
    m->verts[1] = vec3_make( 1, -1, -1);
    m->verts[2] = vec3_make( 1,  1, -1);
    m->verts[3] = vec3_make(-1,  1, -1);
    m->verts[4] = vec3_make(-1, -1,  1);
    m->verts[5] = vec3_make( 1, -1,  1);
    m->verts[6] = vec3_make( 1,  1,  1);
    m->verts[7] = vec3_make(-1,  1,  1);

    /* Winding is counter-clockwise seen from outside.
     *
     * Note on axis naming: the camera sits at z = -dist looking towards +z,
     * so a *smaller* z is *closer*.  The "+z" face below is therefore the
     * one farthest from the viewer.  Nothing depends on the names - culling
     * uses the geometric normal - but reading this table as "front means
     * nearest" will mislead. */
    for (f = 0; f < CUBE_FACES; f++) {
        for (i = 0; i < 4; i++)
            m->faces[f].v[i] = fv[f][i];
        m->faces[f].tex = f;
    }

    {
        int cell = ts / 8;
        if (cell < 2)
            cell = 2;

        for (f = 0; f < CUBE_FACES; f++) {
            apt_texture *t = apt_texture_new(ts, ts);

            m->tex[f] = t;
            if (!t)
                continue;

            for (y = 0; y < ts; y++) {
                for (x = 0; x < ts; x++) {
                    u32 px;
                    int border = (x < 2 || y < 2 || x >= ts - 2 || y >= ts - 2);
                    int grid = ((x % cell) == 0 || (y % cell) == 0);
                    /* darken towards the bottom-right so a face reads as a
                     * surface rather than a flat sticker */
                    int shade = 255 - ((x * 40) / ts) - ((y * 30) / ts);

                    if (border)
                        px = APT_RGB(240, 240, 245);
                    else if (grid)
                        px = APT_RGB(28, 28, 38);
                    else {
                        u32 col = face_colour[f];
                        px = APT_RGB((APT_R(col) * shade) / 255,
                                     (APT_G(col) * shade) / 255,
                                     (APT_B(col) * shade) / 255);
                    }

                    /* the letter, centred, drawn over the middle */
                    {
                        int scale = ts / 20;
                        int cx, cy;

                        if (scale < 1)
                            scale = 1;
                        cx = (x - (ts - 5 * scale) / 2) / scale;
                        cy = (y - (ts - 7 * scale) / 2) / scale;
                        if (cx >= 0 && cx < 5 && cy >= 0 && cy < 7) {
                            if (letter_glyph[f][cy] & (1 << (4 - cx)))
                                px = APT_RGB(255, 255, 255);
                        }
                    }
                    t->pixels[y * ts + x] = px;
                }
            }
        }
    }
}

void cube_free(cube_mesh *m)
{
    int f;

    for (f = 0; f < CUBE_FACES; f++) {
        apt_texture_free(m->tex[f]);
        m->tex[f] = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */

typedef struct {
    int       idx;         /* face index                       */
    apt_fx    depth;       /* average z, for the depth sort    */
    apt_point p[4];        /* projected corners                */
    int       brightness;  /* 0..255 from the light            */
    int       visible;
} face_draw;

int cube_draw(cube_view *v, cube_mesh *m)
{
    vec3      rv[CUBE_VERTS];
    apt_point pv[CUBE_VERTS];
    face_draw list[CUBE_FACES];
    apt_fx    ca, sa, cb, sb, cc, sc;
    int       i, f, drawn = 0;

    if (!v || !v->c || !m)
        return 0;

    ca = fcos(v->ax); sa = fsin(v->ax);
    cb = fcos(v->ay); sb = fsin(v->ay);
    cc = fcos(v->az); sc = fsin(v->az);

    /* 1 + 2.  Rotate and project each vertex exactly once. */
    for (i = 0; i < CUBE_VERTS; i++) {
        vec3 p = m->verts[i];
        vec3 t;
        apt_fx den;
        int sx, sy;

        /* about X */
        t.x = p.x;
        t.y = APT_FX_MUL(p.y, ca) - APT_FX_MUL(p.z, sa);
        t.z = APT_FX_MUL(p.y, sa) + APT_FX_MUL(p.z, ca);

        /* about Y */
        p.x = APT_FX_MUL(t.x, cb) + APT_FX_MUL(t.z, sb);
        p.y = t.y;
        p.z = -APT_FX_MUL(t.x, sb) + APT_FX_MUL(t.z, cb);

        /* about Z */
        t.x = APT_FX_MUL(p.x, cc) - APT_FX_MUL(p.y, sc);
        t.y = APT_FX_MUL(p.x, sc) + APT_FX_MUL(p.y, cc);
        t.z = p.z;

        rv[i] = t;

        den = t.z + APT_FX(v->dist);
        sx = project_div(APT_FX_MUL(t.x, APT_FX(v->scale)), den);
        sy = project_div(APT_FX_MUL(t.y, APT_FX(v->scale)), den);
        pv[i].x = v->cx + sx;
        pv[i].y = v->cy - sy;      /* canvas y grows downwards */
    }

    /* 3.  Light each face and decide whether it faces the camera. */
    for (f = 0; f < CUBE_FACES; f++) {
        vec3 a = rv[m->faces[f].v[0]];
        vec3 b = rv[m->faces[f].v[1]];
        vec3 c = rv[m->faces[f].v[2]];
        vec3 n = vec3_norm(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
        vec3 centre, tocam;
        apt_fx lit, depth;
        int k;

        list[f].idx = f;
        list[f].visible = 1;
        for (k = 0; k < 4; k++)
            list[f].p[k] = pv[m->faces[f].v[k]];

        depth = 0;
        centre.x = centre.y = centre.z = 0;
        for (k = 0; k < 4; k++) {
            vec3 p = rv[m->faces[f].v[k]];

            depth += p.z;
            centre.x += p.x;
            centre.y += p.y;
            centre.z += p.z;
        }
        list[f].depth = depth / 4;
        centre.x /= 4;
        centre.y /= 4;
        centre.z /= 4;

        /* Lambert, two-sided so no face goes fully black. */
        lit = vec3_dot(n, v->light);
        if (lit < 0)
            lit = -lit;
        list[f].brightness = 150 + APT_FX_INT(lit * 105 / APT_FX_ONE);
        if (list[f].brightness > 255)
            list[f].brightness = 255;

        /* Back-face culling: compare the surface normal against the
         * direction to the camera, not against the light. */
        tocam.x = -centre.x;
        tocam.y = -centre.y;
        tocam.z = -APT_FX(v->dist) - centre.z;

        if (vec3_dot(n, tocam) <= 0) {
            if (!v->backface_dark)
                list[f].visible = 0;
            else
                list[f].brightness /= 4;   /* keep it, but darken it */
        }
    }

    /* 4a.  Painter's algorithm: farthest first.  With six faces an insertion
     * sort is plenty, and it avoids depending on a libc qsort. */
    for (i = 1; i < CUBE_FACES; i++) {
        face_draw key = list[i];
        int j = i - 1;

        while (j >= 0 && list[j].depth < key.depth) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }

    /* 4b.  Draw. */
    for (i = 0; i < CUBE_FACES; i++) {
        face_draw *fd = &list[i];
        apt_texture *t = m->tex[fd->idx];
        apt_point q[4];
        int k;

        if (!fd->visible || !t)
            continue;

        for (k = 0; k < 4; k++)
            q[k] = fd->p[k];

        if (v->wireframe) {
            for (k = 0; k < 4; k++) {
                apt_point a = q[k], b = q[(k + 1) & 3];

                apt_line(v->c, a.x, a.y, b.x, b.y, APT_RGB(255, 230, 60));
            }
        } else {
            apt_blit_quad(v->c, t, q);

            /* Shade by laying a black polygon of the same shape over the
             * face at the complementary alpha.  Building six re-shaded
             * textures every frame would cost far more than this. */
            if (fd->brightness < 255) {
                /* Cap the overlay at about half so a face turned away from
                 * the light is dimmed, not obliterated. */
                int shade = (255 - fd->brightness) / 2;

                if (shade > 0) {
                    apt_set_alpha(v->c, shade);
                    apt_fill_poly(v->c, q, 4, APT_BLACK);
                    apt_set_alpha(v->c, 255);
                }
            }
        }
        drawn++;
    }

    return drawn;
}
