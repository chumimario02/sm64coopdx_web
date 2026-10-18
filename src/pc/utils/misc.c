#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <PR/ultratypes.h>
#include <stdbool.h>
#include <time.h>
#include <float.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <dirent.h>
#include "pc/mods/mod_import.h"
#include "pc/mods/mods.h"
#include "pc/mods/mods_utils.h"
#include "pc/djui/djui_popup.h"
#endif
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <direct.h>
#endif
#include <ctype.h>

#include "misc.h"

#include "course_table.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/save_file.h"
#include "engine/math_util.h"
#include "pc/configfile.h"
#include "pc/pc_main.h"
#include "pc/update_checker.h"

float smooth_step(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0) { t = 0; }
    if (t > 1) { t = 1; }
    return t * t * (3.0 - 2.0 * t);
}

void update_all_mario_stars(void) {
    s32 numStars = save_file_get_total_star_count(gCurrSaveFileNum - 1, COURSE_MIN - 1, COURSE_MAX - 1);
    for (s32 i = 0; i < MAX_PLAYERS; i++) {
        gMarioStates[i].numStars = numStars;
    }
}

static void _clock_gettime(struct timespec* clock_time) {
#if !defined _POSIX_MONOTONIC_CLOCK || _POSIX_MONOTONIC_CLOCK < 0
    clock_gettime(CLOCK_REALTIME, clock_time);
#elif _POSIX_MONOTONIC_CLOCK > 0
    clock_gettime(CLOCK_MONOTONIC, clock_time);
#else
    if (clock_gettime(CLOCK_MONOTONIC, clock_time))
        clock_gettime(CLOCK_REALTIME, clock_time);
#endif

#ifdef DEVELOPMENT
    // give each instance a random offset for testing purposed
    /*
    static s32 randomOffset1 = 0;
    static s32 randomOffset2 = 0;
    if (randomOffset1 == 0) {
        time_t t;
        srand((unsigned)time(&t));
        randomOffset1 = rand();
        randomOffset2 = rand();
    }
    clock_time->tv_sec += randomOffset1;
    clock_time->tv_nsec += randomOffset2;
    */
#endif
}

static u64 clock_elapsed_ns(void) {
    static bool sClockInitialized = false;
    static u64 clock_start_ns;
    if (!sClockInitialized) {
        struct timespec clock_start;
        _clock_gettime(&clock_start);
        clock_start_ns = ((u64)clock_start.tv_sec) * 1000000000 + ((u64)clock_start.tv_nsec);
        sClockInitialized = true;
    }

    struct timespec clock_current;
    _clock_gettime(&clock_current);

    u64 clock_current_ns = ((u64)clock_current.tv_sec) * 1000000000 + ((u64)clock_current.tv_nsec);
    return (clock_current_ns - clock_start_ns);
}

f32 clock_elapsed(void) {
    return (clock_elapsed_ns() / 1000000000.0f);
}

f64 clock_elapsed_f64(void) {
    return (clock_elapsed_ns() / 1000000000.0);
}

u32 clock_elapsed_ticks(void) {
    return (clock_elapsed_ns() * 3 / 100000000);
}

bool clock_is_date(u8 month, u8 day) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    return tm_info->tm_mon == month - 1 && tm_info->tm_mday == day;
}

// delay functions lack accuracy sometimes due to os scheduling
// busy-waiting is bad practice but it's very accurate so we use a hybrid
void precise_delay_f64(f64 delaySec) {
#ifdef __EMSCRIPTEN__
    // En el navegador no podemos dormir el hilo: el ritmo lo lleva el bucle
    // principal (web_main_loop_iteration en pc_main.c). Esperar aqui solo
    // quemaria CPU y congelaria la pestana.
    (void)delaySec;
#else
    const f64 sleepMargin = 0.002; // 2 ms margin before we switch to busy-waiting

    f64 start = clock_elapsed_f64();
    f64 end = start + delaySec;

    // sleep until we're ~2ms away from the target
    for (f64 remaining = end - clock_elapsed_f64(); remaining > sleepMargin; remaining = end - clock_elapsed_f64()) {
        u32 sleepMs = (u32) ((remaining - sleepMargin) * 1000.0);
        if (sleepMs < 1) { break; } // not enough time to sleep
        gWindowApi->delay(sleepMs);
    }

    // busy-wait until the target time is hit
    while (clock_elapsed_f64() < end);
#endif
}

void file_get_line(char* buffer, size_t maxLength, FILE* fp) {
    char* initial = buffer;

    char c = fgetc(fp);
    while (!feof(fp) && c != '\n') {
        // make sure it's printable
        if (c < ' ' || c > '~') { goto next_get; }

        // parse new line escape code
        if (c == '\\') {
            c = fgetc(fp);
            if (feof(fp)) { break; }
            if (c == 'n') {
                if ((size_t)(buffer - initial) < (maxLength - 1)) {
                    *buffer++ = '\n';
                }
                goto next_get;
            }
        }

        // found new line
        if (c == '\n') { break; }

        // append to buffer
        if ((size_t)(buffer - initial) < (maxLength - 1)) {
            *buffer++ = c;
        }

next_get:
        c = fgetc(fp);
    }

    *buffer = '\0';
}

/////////////////

f32 delta_interpolate_f32(f32 a, f32 b, f32 delta) {
    return a * (1.0f - delta) + b * delta;
}

s32 delta_interpolate_s32(s32 a, s32 b, f32 delta) {
    return a * (1.0f - delta) + b * delta;
}

void delta_interpolate_vec3f(VEC_OUT Vec3f res, Vec3f a, Vec3f b, f32 delta) {
    f32 antiDelta = 1.0f - delta;
    res[0] = ((a[0] * antiDelta) + (b[0] * delta));
    res[1] = ((a[1] * antiDelta) + (b[1] * delta));
    res[2] = ((a[2] * antiDelta) + (b[2] * delta));
}

void delta_interpolate_vec3s(VEC_OUT Vec3s res, Vec3s a, Vec3s b, f32 delta) {
    f32 antiDelta = 1.0f - delta;
    res[0] = ((a[0] * antiDelta) + (b[0] * delta));
    res[1] = ((a[1] * antiDelta) + (b[1] * delta));
    res[2] = ((a[2] * antiDelta) + (b[2] * delta));
}

void delta_interpolate_normal(s8* res, s8* a, s8* b, f32 delta) {
    f32 antiDelta = 1.0f - delta;
    res[0] = ((a[0] * antiDelta) + (b[0] * delta));
    res[1] = ((a[1] * antiDelta) + (b[1] * delta));
    res[2] = ((a[2] * antiDelta) + (b[2] * delta));
}

void delta_interpolate_rgba(u8* res, u8* a, u8* b, f32 delta) {
    f32 antiDelta = 1.0f - delta;
    res[0] = ((a[0] * antiDelta) + (b[0] * delta));
    res[1] = ((a[1] * antiDelta) + (b[1] * delta));
    res[2] = ((a[2] * antiDelta) + (b[2] * delta));
    res[3] = ((a[3] * antiDelta) + (b[3] * delta));
}

static f32 get_quat_compo_abs(f32 xPiece, f32 yPiece, f32 zPiece) {
    return sqrt((1.0f + xPiece + yPiece + zPiece) * 0.25f);
}

static u8 float_ge_with_nan_check(f32 a, f32 b) {
    return !isnan(a) && (isnan(b) || a >= b);
}

// this function expects an orthonormal rotation matrix
static void rot_mat_to_rot_quat(Vec4f q, Vec3f a[3]) {
    // get absolute value of coefficients
    q[0] = get_quat_compo_abs(a[0][0], a[1][1], a[2][2]);
    q[1] = get_quat_compo_abs(a[0][0], -a[1][1], -a[2][2]);
    q[2] = get_quat_compo_abs(-a[0][0], a[1][1], -a[2][2]);
    q[3] = get_quat_compo_abs(-a[0][0], -a[1][1], a[2][2]);

    // find the coefficient with greatest magnitude
    // NaN checks are because of possible square root of negative number in get_quat_compo_abs
    int maxCompoMagCase = float_ge_with_nan_check(q[0], q[1])
        ? float_ge_with_nan_check(q[0], q[2])
            ? float_ge_with_nan_check(q[0], q[3])
                ? 0
                : 3
            : float_ge_with_nan_check(q[2], q[3])
                ? 2
                : 3
        : float_ge_with_nan_check(q[1], q[2])
            ? float_ge_with_nan_check(q[1], q[3])
                ? 1
                : 3
            : float_ge_with_nan_check(q[2], q[3])
                ? 2
                : 3;

    // adjust signs of coefficients; base on greatest magnitude to improve float accuracy
    f32 divFactor = 0;
    switch (maxCompoMagCase) {
        case 0:
            divFactor = 0.25f / q[0];
            q[1] = (a[1][2] - a[2][1]) * divFactor;
            q[2] = (a[2][0] - a[0][2]) * divFactor;
            q[3] = (a[0][1] - a[1][0]) * divFactor;
            return;

        case 1:
            divFactor = 0.25f / q[1];
            q[0] = (a[1][2] - a[2][1]) * divFactor;
            q[2] = (a[1][0] + a[0][1]) * divFactor;
            q[3] = (a[2][0] + a[0][2]) * divFactor;
            return;

        case 2:
            divFactor = 0.25f / q[2];
            q[0] = (a[2][0] - a[0][2]) * divFactor;
            q[1] = (a[1][0] + a[0][1]) * divFactor;
            q[3] = (a[2][1] + a[1][2]) * divFactor;
            return;

        case 3:
            divFactor = 0.25f / q[3];
            q[0] = (a[0][1] - a[1][0]) * divFactor;
            q[1] = (a[2][0] + a[0][2]) * divFactor;
            q[2] = (a[2][1] + a[1][2]) * divFactor;
            return;
    }
}

static void rot_quat_to_mtx_rot(Vec3f a[3], Vec4f q) {
    f32 dq0s = 2.0f * sqr(q[0]), dq1s = 2.0f * sqr(q[1]), dq2s = 2.0f * sqr(q[2]),
        dq3s = 2.0f * sqr(q[3]), dq12 = 2.0f * q[1] * q[2], dq03 = 2.0f * q[0] * q[3],
        dq13 = 2.0f * q[1] * q[3], dq02 = 2.0f * q[0] * q[2], dq23 = 2.0f * q[2] * q[3],
        dq01 = 2.0f * q[0] * q[1];

    a[0][0] = dq0s + dq1s - 1.0f;
    a[0][1] = dq12 + dq03;
    a[0][2] = dq13 - dq02;
    a[1][0] = dq12 - dq03;
    a[1][1] = dq0s + dq2s - 1.0f;
    a[1][2] = dq23 + dq01;
    a[2][0] = dq13 + dq02;
    a[2][1] = dq23 - dq01;
    a[2][2] = dq0s + dq3s - 1.0f;
}

// rotation quaternion spherical linear interpolation
static void rot_quat_slerp(Vec4f out, Vec4f a, Vec4f b, f32 t) {
    // credit where it's due:
    // Martin John Baker
    // https://www.euclideanspace.com/maths/algebra/realNormedAlgebra/quaternions/slerp/index.htm

    f32 halfTh = 0;
    f32 halfSin = 0;
    f32 st = 0;
    f32 sat = 0;
    f32 halfCos = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];

    memcpy(out, b, sizeof(f32) * 4);

    // if the abs of cos is 1, then there is no rotation
    if (fabs(halfCos) >= 1.0f) {
        return;
    }

    // if cos is negative, rotation takes long path; invert to take short path
    if (halfCos < 0.0f) {
        out[0] *= -1.0f;
        out[1] *= -1.0f;
        out[2] *= -1.0f;
        out[3] *= -1.0f;
        halfCos *= -1.0f;
    }

    halfTh = acos(halfCos);
    halfSin = sqrt(1.0f - sqr(halfCos));

    if (halfSin == 0.0f) {
        // this shouldn't happen, but float imprecision can make it happen anyway
        halfSin = FLT_EPSILON;
    }

    sat = sin((1 - t) * halfTh) / halfSin;
    st = sin(t * halfTh) / halfSin;
    out[0] = a[0] * sat + out[0] * st;
    out[1] = a[1] * sat + out[1] * st;
    out[2] = a[2] * sat + out[2] * st;
    out[3] = a[3] * sat + out[3] * st;
}

// removes scaling from the shear value
inline static f32 unmat_unscale_shear(f32 shear, f32 scale) {
    if (scale == 0.0f) {
        // assume no shear
        return 0.0f;
    }

    return shear / scale;
}

// matrix decomposition code is brought over from "unmatrix.c" from Graphics Gems II
// Author: Spencer W. Thomas of University of Michigan
// additional Graphics Gems II code by Andrew Glassner and Rod G. Bogart
// thanks to fgsfds for informing me of this
// http://www.realtimerendering.com/resources/GraphicsGems/gemsii/unmatrix.c
//
// matrix perspective is not used in SM64, so those indices are stripped from the output parameter
// return value was related to if matrix was non-singular, which was necessary for perspective
// since perspective is not used, the return value is also stripped
//
// additionally, rotation is not converted to euler angles
// instead, it is converted to a quaternion to avoid gimbal lock
//
// tranfs is returned as follows:
// scale(x, y, z), shear(xy, xz, zy), rotation(a, b, c, d), translation(x, y, z)
OPTIMIZE_O3 static void unmatrix(Mtx * mat, f32 tranfs[13]) {
    int i = 0;
    Vec3f axisVecs[3] = { 0 };
    Vec3f yzCross = { 0 };

    Mtx locMat = *mat;

    // matrix normalization skipped since all SM64 matrices are normalized

    // perspective is not used in SM64 matrices, so the steps for it are skipped

    for (i = 0; i < 3; ++i) {
        // translation (position)
        tranfs[10 + i] = locMat.m[3][i];

        // axis vector
        memcpy(axisVecs[i], locMat.m[i], sizeof(f32) * 3);
    }

    // X-scale
    tranfs[0] = vec3f_length(axisVecs[0]);

    // normalize X-axis vector
    if (tranfs[0] == 0.0f) {
        axisVecs[0][0] = 1.0f;
        axisVecs[0][1] = 0.0f;
        axisVecs[0][2] = 0.0f;
    } else {
        for (i = 0; i < 3; ++i) {
            axisVecs[0][i] /= tranfs[0];
        }
    }

    // XY-shear
    tranfs[3] = vec3f_dot(axisVecs[0], axisVecs[1]);

    // orthogonalize Y-axis vector to X-axis vector
    vec3f_combine(axisVecs[1], axisVecs[1], axisVecs[0], 1.0f, -tranfs[3]);

    // Y-scale
    tranfs[1] = vec3f_length(axisVecs[1]);

    // normalize Y-axis vector
    if (tranfs[1] == 0.0f) {
        axisVecs[1][0] = 0.0f;
        axisVecs[1][1] = 1.0f;
        axisVecs[1][2] = 0.0f;
    } else {
        for (i = 0; i < 3; ++i) {
            axisVecs[1][i] /= tranfs[1];
        }
    }

    // unscale XY-shear
    tranfs[3] = unmat_unscale_shear(tranfs[3], tranfs[1]);

    // XZ-shear
    tranfs[4] = vec3f_dot(axisVecs[0], axisVecs[2]);

    // orthogonalize Z-axis vector to X-axis vector
    vec3f_combine(axisVecs[2], axisVecs[2], axisVecs[0], 1.0f, -tranfs[4]);

    // ZY-shear
    tranfs[5] = vec3f_dot(axisVecs[1], axisVecs[2]);

    // orthogonalize Z-axis vector to Y-axis vector
    vec3f_combine(axisVecs[2], axisVecs[2], axisVecs[1], 1.0f, -tranfs[5]);

    // Z-scale
    tranfs[2] = vec3f_length(axisVecs[2]);

    // normalize Z-axis vector
    if (tranfs[2] == 0.0f) {
        axisVecs[2][0] = 0.0f;
        axisVecs[2][1] = 0.0f;
        axisVecs[2][2] = 1.0f;
    } else {
        for (i = 0; i < 3; ++i) {
            axisVecs[2][i] /= tranfs[2];
        }
    }

    // unscale remaining shears
    tranfs[4] = unmat_unscale_shear(tranfs[4], tranfs[2]);
    tranfs[5] = unmat_unscale_shear(tranfs[5], tranfs[2]);

    // check if coordinate system needs to be flipped
    vec3f_cross(yzCross, axisVecs[1], axisVecs[2]);
    if (vec3f_dot(axisVecs[0], yzCross) < 0.0f) {
        for (i = 0; i < 3; ++i) {
            tranfs[i] *= -1.0f;
            vec3f_mul(axisVecs[i], -1.0f);
        }
    }

    // rotation
    rot_mat_to_rot_quat(tranfs + 6, axisVecs);
}

// builds a transformation matrix from a decomposed sequence from unmatrix
// see unmatrix for what tranfs means
static void rematrix(Mtx * mat, f32 tranfs[13]) {
    int i;
    Vec3f rotAxes[3] = { 0 };
    Mat4 rotMat = { 0 };

    // start with the identity matrix
    for (i = 0; i < 4; ++i) {
        int j;

        mat->m[i][i] = 1.0f;
        for (j = 3; j > i; --j) {
            mat->m[i][j] = mat->m[j][i] = 0.0f;
        }
    }

    // scale
    for (i = 0; i < 3; ++i) {
        vec3f_mul(mat->m[i], tranfs[i]);
    }

    // shear
    mat->m[1][0] = mat->m[1][1] * tranfs[3];
    mat->m[2][0] = mat->m[2][2] * tranfs[4];
    mat->m[2][1] = mat->m[2][2] * tranfs[5];

    // rotate
    // get the rotation matrix for the quat
    rot_quat_to_mtx_rot(rotAxes, tranfs + 6);

    // transfer to the Mat4 struct
    for (i = 0; i < 3; ++i) {
        memcpy(rotMat[i], rotAxes[i], sizeof(f32) * 3);
        rotMat[i][3] = rotMat[3][i] = 0.0f;
    }
    rotMat[3][3] = 1.0f;

    // apply the rotation
    // this is technically abuse of Mat4 vs. Mtx, but coop doesn't target N64 anyway
    mtxf_mul(mat->m, mat->m, rotMat);

    // translate
    for (i = 0; i < 3; ++i) {
        mat->m[3][i] = tranfs[10 + i];
    }
}

inline static void delta_interpolate_mtx_accurate(Mtx* out, Mtx* a, Mtx* b, f32 delta) {
    int i = 0;
    f32 matTranfsA[13] = { 0 };
    f32 matTranfsB[13] = { 0 };

    f32 antiDelta = 1.0f - delta;

    unmatrix(a, matTranfsA);
    unmatrix(b, matTranfsB);

    // skip rot quat
    for (i = 0; i < 6; ++i) {
        matTranfsB[i] = matTranfsA[i] * antiDelta + matTranfsB[i] * delta;
    }
    for (i = 10; i < 13; ++i) {
        matTranfsB[i] = matTranfsA[i] * antiDelta + matTranfsB[i] * delta;
    }

    rot_quat_slerp(matTranfsB + 6, matTranfsA + 6, matTranfsB + 6, delta);

    rematrix(out, matTranfsB);
}

void delta_interpolate_mtx(Mtx* out, Mtx* a, Mtx* b, f32 delta) {
    // HACK: Limit accurate interpolation to 64-bit builds
    if (sizeof(void*) > 4) {
        if (configInterpolationMode) {
            delta_interpolate_mtx_accurate(out, a, b, delta);
            return;
        }
    }

    // this isn't the right way to do things.
    f32 antiDelta = 1.0f - delta;
    for (s32 i = 0; i < 4; i++) {
        for (s32 j = 0; j < 4; j++) {
            out->m[i][j] = (a->m[i][j] * antiDelta) + (b->m[i][j] * delta);
        }
    }
}

void detect_and_skip_mtx_interpolation(Mtx** mtxPrev, Mtx** mtx) {
    // if the matrix has changed "too much", then skip interpolation
    const f32 minDot = sqrt(2.0f) / -3.0f;
    Vec3f prevX; vec3f_copy(prevX, (f32*)(*mtxPrev)->m[0]); vec3f_normalize(prevX);
    Vec3f prevY; vec3f_copy(prevY, (f32*)(*mtxPrev)->m[1]); vec3f_normalize(prevY);
    Vec3f prevZ; vec3f_copy(prevZ, (f32*)(*mtxPrev)->m[2]); vec3f_normalize(prevZ);

    Vec3f nextX; vec3f_copy(nextX, (f32*)(*mtx)->m[0]); vec3f_normalize(nextX);
    Vec3f nextY; vec3f_copy(nextY, (f32*)(*mtx)->m[1]); vec3f_normalize(nextY);
    Vec3f nextZ; vec3f_copy(nextZ, (f32*)(*mtx)->m[2]); vec3f_normalize(nextZ);

    f32 dotX = vec3f_dot(prevX, nextX);
    f32 dotY = vec3f_dot(prevY, nextY);
    f32 dotZ = vec3f_dot(prevZ, nextZ);

    if ((dotX < minDot) || (dotY < minDot) || (dotZ < minDot)) {
        *mtx = *mtxPrev;
    }
}

void str_seperator_concat(char *output_buffer, int buffer_size, char** strings, int num_strings, char* seperator) {
    // empty buffer
    memset(output_buffer, 0, buffer_size);
    if (num_strings <= 0) { return; }

    // Calculate the total length of all strings
    int string_length[num_strings];
    int total_length = 0;
    for (int i = 0; i < num_strings; i++) {
        string_length[i] = strlen(strings[i]);
        total_length += string_length[i];
    }

    // get the seperator length
    int seperator_length = strlen(seperator);
    int seperators_length = (num_strings - 1) * seperator_length;
    if (seperators_length + 8 < buffer_size) {
        // Shorten the largest string over and over until we fit
        while (total_length + seperators_length >= buffer_size) {
            int* largest = NULL;
            for (int i = 0; i < num_strings; i++) {
                if (largest == NULL || string_length[i] >= *largest) {
                    largest = &string_length[i];
                }
            }
            if (largest == NULL || *largest == 0) { break; }
            *largest = *largest - 1;
            total_length--;
        }
    }

    // Fill the buffer
    int buffer_index = 0;
    for (int i = 0; i < num_strings; i++) {
        // Concat string
        int amount = MIN(buffer_size - buffer_index, string_length[i] + 1);
        if (amount <= 0) { break; }
        snprintf(&output_buffer[buffer_index], amount, "%s", strings[i]);
        buffer_index += string_length[i];

        // Concat seperator
        if (i != (num_strings - 1)) {
            int amount = MIN(buffer_size - buffer_index, seperator_length + 1);
            if (amount <= 0) { break; }
            snprintf(&output_buffer[buffer_index], amount, "%s", seperator);
            buffer_index += seperator_length;
        }
    }
}

#if defined(__linux__) || defined(__APPLE__)
static s8 launch(const char* program, const char* arg) {
    pid_t pid = fork();

    if (pid < 0) { return -1; }

    if (pid == 0) {
        execlp(program, program, arg, (char*)NULL);
        _exit(127);
    }

    s32 status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
#endif


void open_url(const char* url) {
#ifdef __EMSCRIPTEN__
    // Web port: there is no system browser to launch, so open a new tab. This
    // is what the game's "browse mods"/website buttons do in a browser.
    EM_ASM({
        try { window.open(UTF8ToString($0), '_blank', 'noopener'); }
        catch (e) { console.log('[web] could not open the link: ' + e); }
    }, url);
    return;

#elif defined(_WIN32) || defined(_WIN64) // windows
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);

#elif __linux__ // linux
    launch("xdg-open", url);

#elif __APPLE__ // macOS
    launch("open", url);

#endif
}

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// Browser: there are no system folders to open, so the "open folder" buttons
// (mods, dynos, saves...) become "add files to that folder". The page
// (web/pre.js) shows a picker, copies what was chosen into the game filesystem
// (which lives in IndexedDB, that is, on the player's own PC) and calls
// web_add_files_done() when it is done.
//
// Files the game knows how to import (mods .zip and .lua, palettes .ini) are
// handed to its own importer, mod_import_file(), the very same one used by
// desktop drag and drop.
// ---------------------------------------------------------------------------
#define WEB_ADD_STAGING "_web_add"

static char sWebAddDestino[SYS_MAX_PATH] = { 0 };
static bool sWebAddPendiente = false;

static bool web_es_punto(const char* nombre) {
    return nombre[0] == '.' && (nombre[1] == 0 || (nombre[1] == '.' && nombre[2] == 0));
}

static void web_copiar_archivo(const char* src, const char* dst) {
    FILE* origen = fopen(src, "rb");
    if (origen == NULL) { return; }

    FILE* copia = fopen(dst, "wb");
    if (copia == NULL) { fclose(origen); return; }

    static char bloque[65536];
    size_t leidos;
    while ((leidos = fread(bloque, 1, sizeof(bloque), origen)) > 0) {
        fwrite(bloque, 1, leidos, copia);
    }
    fclose(origen);
    fclose(copia);
}

static void web_copiar_arbol(const char* src, const char* dst) {
    DIR* dir = opendir(src);
    if (dir == NULL) { return; }

    mkdir(dst, 0777);

    struct dirent* entrada;
    while ((entrada = readdir(dir)) != NULL) {
        if (web_es_punto(entrada->d_name)) { continue; }

        char hijoOrigen[SYS_MAX_PATH] = { 0 };
        char hijoDestino[SYS_MAX_PATH] = { 0 };
        if (snprintf(hijoOrigen, sizeof(hijoOrigen), "%s/%s", src, entrada->d_name) >= (int)sizeof(hijoOrigen)) { continue; }
        if (snprintf(hijoDestino, sizeof(hijoDestino), "%s/%s", dst, entrada->d_name) >= (int)sizeof(hijoDestino)) { continue; }

        struct stat info;
        if (stat(hijoOrigen, &info) == 0 && S_ISDIR(info.st_mode)) {
            web_copiar_arbol(hijoOrigen, hijoDestino);
        } else {
            web_copiar_archivo(hijoOrigen, hijoDestino);
        }
    }
    closedir(dir);
}

static void web_borrar_arbol(const char* ruta) {
    DIR* dir = opendir(ruta);
    if (dir != NULL) {
        struct dirent* entrada;
        while ((entrada = readdir(dir)) != NULL) {
            if (web_es_punto(entrada->d_name)) { continue; }
            char hijo[SYS_MAX_PATH] = { 0 };
            if (snprintf(hijo, sizeof(hijo), "%s/%s", ruta, entrada->d_name) >= (int)sizeof(hijo)) { continue; }
            struct stat info;
            if (stat(hijo, &info) == 0 && S_ISDIR(info.st_mode)) {
                web_borrar_arbol(hijo);
            } else {
                remove(hijo);
            }
        }
        closedir(dir);
    }
    rmdir(ruta);
}

// Called from web/pre.js once it has copied the chosen files into the staging
// folder. It only sets the flag: the real work happens in
// web_add_files_process() from the game loop, so DJUI and the mods are touched
// in the usual context.
EMSCRIPTEN_KEEPALIVE void web_add_files_done(void) {
    sWebAddPendiente = true;
}

void web_add_files_process(void) {
    if (!sWebAddPendiente) { return; }
    sWebAddPendiente = false;

    char staging[SYS_MAX_PATH] = { 0 };
    snprintf(staging, sizeof(staging), "%s", fs_get_write_path(WEB_ADD_STAGING));

    DIR* dir = opendir(staging);
    if (dir == NULL) {
        djui_popup_create("Could not read the mods you picked.", 2);
        return;
    }

    int carpetas = 0;
    int archivos = 0;
    bool huboMod = false;

    struct dirent* entrada;
    while ((entrada = readdir(dir)) != NULL) {
        if (web_es_punto(entrada->d_name)) { continue; }

        char ruta[SYS_MAX_PATH] = { 0 };
        if (snprintf(ruta, sizeof(ruta), "%s/%s", staging, entrada->d_name) >= (int)sizeof(ruta)) { continue; }

        struct stat info;
        if (stat(ruta, &info) != 0) { continue; }

        if (S_ISDIR(info.st_mode)) {
            // A plain folder (an already unzipped mod) is copied to the chosen folder.
            char destino[SYS_MAX_PATH] = { 0 };
            if (snprintf(destino, sizeof(destino), "%s/%s", sWebAddDestino, entrada->d_name) >= (int)sizeof(destino)) { continue; }
            web_copiar_arbol(ruta, destino);
            carpetas++;
            huboMod = true;
        } else if (path_ends_with(entrada->d_name, ".zip") || path_ends_with(entrada->d_name, ".lua") ||
                   path_ends_with(entrada->d_name, ".luac") || path_ends_with(entrada->d_name, ".ini")) {
            // Mods and palettes: let the game's own importer handle them (it reports back).
            mod_import_file(ruta);
            archivos++;
            huboMod = true;
        } else {
            // Anything else (dynos, configs...) is just copied over.
            char destino[SYS_MAX_PATH] = { 0 };
            if (snprintf(destino, sizeof(destino), "%s/%s", sWebAddDestino, entrada->d_name) >= (int)sizeof(destino)) { continue; }
            web_copiar_archivo(ruta, destino);
            archivos++;
        }
    }
    closedir(dir);

    web_borrar_arbol(staging);

    if (huboMod && carpetas > 0) {
        mods_refresh_local();
        mods_update_selectable();
    }

    if (carpetas + archivos > 0) {
        char msg[SYS_MAX_PATH];
        snprintf(msg, sizeof(msg),
                 "Added %d folder(s) and %d file(s).\nPress Refresh to see them, then reload the page (F5) to activate them.",
                 carpetas, archivos);
        djui_popup_create(msg, 3);
    }
}

void web_add_files_dialog(const char* destino) {
    snprintf(sWebAddDestino, sizeof(sWebAddDestino), "%s", destino);

    char staging[SYS_MAX_PATH] = { 0 };
    snprintf(staging, sizeof(staging), "%s", fs_get_write_path(WEB_ADD_STAGING));
    mkdir(staging, 0777);

    sWebAddDestino[sizeof(sWebAddDestino) - 1] = 0;

    EM_ASM({
        if (typeof globalThis.__sm64SubirMods === 'function') {
            globalThis.__sm64SubirMods(UTF8ToString($0));
        } else {
            console.log('[web-mods] the upload helper (web/pre.js) is missing');
        }
    }, staging);
}
#endif // __EMSCRIPTEN__

void open_folder(const char* path) {
#ifdef __EMSCRIPTEN__
    // In a browser there is no "open the folder": ask for the files instead.
    web_add_files_dialog(path);
    return;
#endif
#if defined(_WIN32) || defined(_WIN64) // windows
    _mkdir(path);
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);

#elif __linux__ // linux
    mkdir(path, 0777);
    launch("xdg-open", path);

#elif __APPLE__ // macOS
    mkdir(path, 0777);
    launch("open", path);
#endif
}

const char *strstr_lowercased(const char *haystack, const char *needle) {
    // sanity check
    if (!*needle) {
        return haystack;
    }

    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            ++h;
            ++n;
        }

        if (!*n) {
            return haystack;
        }

        ++haystack;
    }

    return NULL;
}

static char *get_update_path(void) {
#ifdef _WIN32
    char updateExecFilename[] = "coopdx_updater.exe";
#else
    char updateExecFilename[] = "coopdx_updater";
#endif
    static char sUpdateExecFilePath[SYS_MAX_PATH];
    // this may truncate as sys_exe_path_dir is allocated to be of size SYS_MAX_SIZE, nonetheless such a limit should not be hit during normal use.
    snprintf(sUpdateExecFilePath, sizeof(sUpdateExecFilePath), "%s%s%s", sys_exe_path_dir(), PATH_SEPARATOR, updateExecFilename);
    return sUpdateExecFilePath;
}

bool can_update_game(void) {
    // the file is not guaranteed to exist, so make sure we have the updater installed
    return fs_sys_file_exists(get_update_path()) && gUpdateMessage;
}

void update_game(void) {
    const char *updateExecFilePath = get_update_path();

#ifdef _WIN32
    STARTUPINFOA si = { 0 };
    PROCESS_INFORMATION pi = { 0 };

    si.cb = sizeof(si);

    char commandBuf[SYS_MAX_PATH];
    // this can truncate, but under normal use, SYS_MAX_PATH should not ever be filled up
    snprintf(commandBuf, sizeof(commandBuf), "%s --game-update", updateExecFilePath);

    if (CreateProcessA(NULL, commandBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    exit(0);
#else
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    execl(updateExecFilePath, "coopdx_updater", "--game-update", NULL);
    exit(1);
#endif
}