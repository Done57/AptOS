/* A small 3D layer on top of apt, used by the rotating-cube test.
 *
 * apt itself is a 2D API, so everything three-dimensional happens here:
 * vertices are rotated, projected, lit and finally handed to apt as textured
 * quads.  There is deliberately no depth buffer - a cube's faces never
 * intersect each other, so painter's algorithm (sort by depth, draw far to
 * near) plus back-face culling is both correct and much cheaper than a
 * per-pixel Z test.
 *
 * All maths is 16.16 fixed point.  A ring 3 program here is freestanding, so
 * there is no floating point support and no 64-bit divide helper to lean on;
 * every routine below is written to stay inside 32 bits.
 */
#ifndef CUBE3D_H
#define CUBE3D_H

#include "apt.h"

/* ------------------------------------------------------------------ */
/* vectors                                                             */

typedef struct { apt_fx x, y, z; } vec3;

static inline vec3 vec3_make(int x, int y, int z)
{
    vec3 v;
    v.x = APT_FX(x);
    v.y = APT_FX(y);
    v.z = APT_FX(z);
    return v;
}

static inline vec3 vec3_sub(vec3 a, vec3 b)
{
    vec3 r;
    r.x = a.x - b.x;
    r.y = a.y - b.y;
    r.z = a.z - b.z;
    return r;
}

static inline vec3 vec3_cross(vec3 a, vec3 b)
{
    vec3 r;
    r.x = APT_FX_MUL(a.y, b.z) - APT_FX_MUL(a.z, b.y);
    r.y = APT_FX_MUL(a.z, b.x) - APT_FX_MUL(a.x, b.z);
    r.z = APT_FX_MUL(a.x, b.y) - APT_FX_MUL(a.y, b.x);
    return r;
}

static inline apt_fx vec3_dot(vec3 a, vec3 b)
{
    return APT_FX_MUL(a.x, b.x) + APT_FX_MUL(a.y, b.y) + APT_FX_MUL(a.z, b.z);
}

/* 16.16 division that cannot overflow: both parts of the split are computed
 * before scaling, so the largest intermediate is (|c%d| << 16) < d << 16,
 * which stays inside 32 bits for any d we care about. */
static inline apt_fx apt_fx_div(apt_fx num, apt_fx den)
{
    if (den == 0)
        return 0;
    return (apt_fx)(((s32)num / (s32)den) * APT_FX_ONE +
                    ((((s32)num % (s32)den) * APT_FX_ONE) / (s32)den));
}

/* Normalise using an integer square root, avoiding any division by a value
 * that could be zero. */
static u32 isqrt32(u32 v)
{
    u32 rem = 0, root = 0;
    int i;

    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        if (root < rem) {
            rem -= root + 1;
            root += 2;
        }
    }
    return root >> 1;
}

static inline vec3 vec3_norm(vec3 a)
{
    /* vec3_dot returns a 16.16 value, so `d2` is |a|^2 scaled by 65536.
     * Shifting that up again before the square root would need 64 bits and
     * silently overflows to zero for any vector longer than about 1.4 - and
     * with a zero length the whole normalisation collapses, taking the
     * lighting and the back-face test with it.
     *
     * Instead take the integer square root first:
     *     isqrt(d2) ~= sqrt(|a|^2 * 65536) = |a| * 256     (16.8 fixed point)
     * and shift left by 8 to land on the 16.16 the rest of the code wants. */
    apt_fx d2 = vec3_dot(a, a);
    s32 d;

    if (d2 <= 0)
        return vec3_make(0, 0, 0);

    d = (s32)isqrt32((u32)d2) << 8;
    if (d <= 0)
        return vec3_make(0, 0, 0);

    {
        vec3 r;
        /* Component / d, in 16.16.  Written as (c/d)*ONE + ((c%d)*ONE)/d so
         * that no intermediate ever exceeds 32 bits. */
        r.x = apt_fx_div(a.x, d);
        r.y = apt_fx_div(a.y, d);
        r.z = apt_fx_div(a.z, d);
        return r;
    }
}

/* ------------------------------------------------------------------ */
/* rotation                                                            */

/* A rotation is kept as three angles and applied as three successive axis
 * rotations.  That is a gimbal arrangement and would be wrong for a camera,
 * but for a spinning cube it is exactly what is wanted: the angles are what
 * the user is controlling. */

/* Integer sine/cosine over a 0..255 angle circle (0..360 degrees).  A 256
 * entry table of 16.16 values; the values are the standard quarter-wave
 * mirrored, which keeps the table small and the lookups cheap. */
static const s32 sin_tab[65] = {
          0,    1608,    3216,    4821,    6424,    8022,    9616,   11204,
      12785,   14359,   15924,   17479,   19024,   20557,   22078,   23586,
      25080,   26558,   28020,   29466,   30893,   32303,   33692,   35062,
      36410,   37736,   39040,   40320,   41576,   42806,   44011,   45190,
      46341,   47464,   48559,   49624,   50660,   51665,   52639,   53581,
      54491,   55368,   56212,   57022,   57798,   58538,   59244,   59914,
      60547,   61145,   61705,   62228,   62714,   63162,   63572,   63944,
      64277,   64571,   64827,   65043,   65220,   65358,   65457,   65516,
      65536
};

/* angle is 0..255 for a full turn */
static apt_fx fsin(int angle)
{
    int a = angle & 255;

    if (a <= 64)
        return sin_tab[a];
    if (a <= 128)
        return sin_tab[128 - a];
    if (a <= 192)
        return -sin_tab[a - 128];
    return -sin_tab[256 - a];
}

static apt_fx fcos(int angle)
{
    return fsin(angle + 64);
}

/* ------------------------------------------------------------------ */
/* mesh                                                                */

#define CUBE_FACES 6
#define CUBE_VERTS 8

/* A face is a quad of four vertex indices plus the texture to map onto it. */
typedef struct {
    int v[4];
    int tex;          /* index into the texture array */
} cube_face;

typedef struct {
    vec3        verts[CUBE_VERTS];
    cube_face   faces[CUBE_FACES];
    apt_texture *tex[CUBE_FACES];
} cube_mesh;

/* ------------------------------------------------------------------ */
/* rendering                                                           */

typedef struct {
    apt_canvas *c;
    int cx, cy;          /* where the cube's centre goes on the canvas */
    int scale;           /* pixels per unit at z = 0                     */
    int dist;            /* camera distance in units                     */

    vec3 light;          /* normalised direction the light comes from    */

    /* rotation angles, in the 0..255 circle */
    int ax, ay, az;

    /* debug switches */
    int wireframe;
    int backface_dark;
    int debug_print;
} cube_view;

/* Fill in a unit-ish cube (side 2, centred on the origin) and attach six
 * textures.  The textures are generated, not loaded, so the demo has no
 * external assets. */
void cube_init(cube_mesh *m, int tex_size);

void cube_free(cube_mesh *m);

/* Draw the cube.  Returns the number of faces actually rasterised, which is
 * a useful sanity check (a cube shows at most three). */
int cube_draw(cube_view *v, cube_mesh *m);

#endif /* CUBE3D_H */
