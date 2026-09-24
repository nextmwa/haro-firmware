#include "face_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "font5x7.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

// Physically GPIO11 (SDA) / GPIO10 (SCL) -- see face_display.h's comment on
// face_display_init() for why there's no GPIO_DISPLAY_SDA/SCL define here
// anymore: those pins belong to audio_pipeline's shared bus, not this file.
#define DISPLAY_I2C_ADDR 0x3C
#define WIDTH 128
#define HEIGHT 64

static const char *TAG = "face_display";
static esp_lcd_panel_handle_t s_panel;

// 1bpp framebuffer, WIDTH*HEIGHT/8 bytes.
//
// Byte-packing note (verified against the real ESP-IDF v6.1 source, not
// assumed): esp_lcd_panel_ssd1306.c's panel_ssd1306_draw_bitmap() does NOT
// repack color_data at all -- it computes
//   len = (y_end - y_start) * (x_end - x_start) * bits_per_pixel / 8
// and forwards the buffer byte-for-byte to esp_lcd_panel_io_tx_color(), whose
// I2C implementation (esp_lcd_panel_io_i2c.c: panel_io_i2c_tx_color ->
// panel_io_i2c_tx_buffer) also just blasts the bytes over the wire with no
// transformation. panel_ssd1306_init() puts the controller in SSD1306
// "horizontal addressing mode" (SET_MEMORY_ADDR_MODE = 0x00): after each
// byte write the controller's column address auto-increments, wrapping to
// the next page once it reaches the page's last column. So the byte stream
// the caller must produce is NOT a naive row-major "8 pixels per byte along
// a scanline" bitmap -- it must already be in the SSD1306's native GDDRAM
// layout: WIDTH*HEIGHT/8 bytes ordered page-major/column-minor (8 pages of
// HEIGHT/8, each page WIDTH bytes wide), where bit b (0=LSB) of the byte at
// framebuffer[page*WIDTH + x] is the pixel at (x, page*8 + b) -- i.e. each
// byte is one 8-pixel-tall vertical strip, LSB = topmost row of that strip.
// This is the standard SSD1306 GDDRAM packing used by every other driver for
// this chip (Adafruit_SSD1306, u8g2, etc).
static uint8_t s_framebuffer[WIDTH * HEIGHT / 8];

// --- Pixel-level framebuffer primitive ----------------------------------

static inline void set_pixel(uint8_t *fb, int x, int y)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    fb[(y / 8) * WIDTH + x] |= (uint8_t)(1u << (y % 8));
}

// Every existing shape in this file is additive (set_pixel only) since
// every expression is drawn on a freshly-cleared framebuffer. The die face
// (face_display_show_action() below) is the first shape that needs to
// punch dark pips out of an already-lit white square, hence this pair.
static inline void clear_pixel(uint8_t *fb, int x, int y)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    fb[(y / 8) * WIDTH + x] &= (uint8_t) ~(1u << (y % 8));
}

// --- Drawing primitives, ported from haro/src/haro/face_display.py -------
//
// PIL's Image.rotate() has no C equivalent; `draw_eye()`'s rotated branch
// below ports the "rasterize small, rotate, composite" technique used by
// face_display.py's `_eye()` as manual inverse-rotation sampling: for each
// destination pixel, rotate its coordinate by -angle around the eye's
// centre and test that rotated coordinate against the *unrotated*
// rounded-rectangle equation (nearest-neighbour; face_display.py's version
// anti-aliases via BICUBIC resample, which a 1bpp OLED can't represent
// anyway).

// Point-in-rounded-rect test, in coordinates centered on the rect (px, py
// are offsets from the rect's own center). Mirrors PIL's rounded_rectangle
// geometry: a w x h rect with corner radius r is the union of a
// (w-2r) x h rect, a w x (h-2r) rect, and four quarter-circles of radius r
// at the corners.
static bool point_in_rounded_rect_centered(double px, double py, double w, double h, double r)
{
    double hw = w / 2.0, hh = h / 2.0;
    double ax = fabs(px), ay = fabs(py);
    if (ax > hw || ay > hh) {
        return false;
    }
    double rx = hw - r, ry = hh - r;
    if (ax <= rx || ay <= ry) {
        return true;
    }
    double ddx = ax - rx, ddy = ay - ry;
    return (ddx * ddx + ddy * ddy) <= r * r;
}

// Non-rotated rounded-rectangle fill, centered at (cx, cy).
static void draw_rounded_rect(uint8_t *fb, int cx, int cy, int w, int h, int r)
{
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    int hw = w / 2, hh = h / 2;
    for (int y = -hh; y <= hh; y++) {
        for (int x = -hw; x <= hw; x++) {
            if (point_in_rounded_rect_centered(x, y, w, h, r)) {
                set_pixel(fb, cx + x, cy + y);
            }
        }
    }
}

// Cozmo "leaf" eye: a rounded-rectangle, optionally tilted by angle_deg
// (matching face_display.py's `_eye(image, cx, cy, w, h, radius, angle)`).
static void draw_eye(uint8_t *fb, int cx, int cy, int w, int h, int r, double angle_deg)
{
    if (r > w / 2) {
        r = w / 2;
    }
    if (r > h / 2) {
        r = h / 2;
    }
    if (angle_deg == 0.0) {
        draw_rounded_rect(fb, cx, cy, w, h, r);
        return;
    }
    double theta = angle_deg * M_PI / 180.0;
    double cos_t = cos(theta), sin_t = sin(theta);
    // Bounding radius of the rotated shape can't exceed the rect's diagonal.
    int half = (int)ceil(sqrt((double)w * w + (double)h * h) / 2.0) + 1;
    for (int ly = -half; ly <= half; ly++) {
        for (int lx = -half; lx <= half; lx++) {
            // Inverse-rotate the destination pixel back into the eye's
            // unrotated local space and test it there.
            double rx = lx * cos_t - ly * sin_t;
            double ry = lx * sin_t + ly * cos_t;
            if (point_in_rounded_rect_centered(rx, ry, w, h, r)) {
                set_pixel(fb, cx + lx, cy + ly);
            }
        }
    }
}

// Filled "upside-down U" / arch eye (classic Anki Cozmo "looking around"
// eye shape): rounded dome on top, flat bottom, straight sides -- distinct
// from draw_eye()'s pill shape, which is rounded at both ends. Solid fill,
// not an outline.
static bool point_in_arch_centered(double px, double py, double w, double h)
{
    double hw = w / 2.0, hh = h / 2.0;
    if (fabs(px) > hw || py > hh || py < -hh) {
        return false;
    }
    if (py >= 0.0) {
        return true; // lower half: plain rectangle
    }
    double nx = px / hw, ny = py / hh; // upper half: inside the top semi-ellipse
    return (nx * nx + ny * ny) <= 1.0;
}

static void draw_eye_arch(uint8_t *fb, int cx, int cy, int w, int h)
{
    int hw = w / 2, hh = h / 2;
    for (int y = -hh; y <= hh; y++) {
        for (int x = -hw; x <= hw; x++) {
            if (point_in_arch_centered(x, y, w, h)) {
                set_pixel(fb, cx + x, cy + y);
            }
        }
    }
}

// Mirror of the arch eye across its horizontal axis: flat bottom becomes
// flat top, dome now faces down into a "\_/" crescent -- Cozmo's
// content/happy eye shape (a filled smile-eye, not just a stroked arc).
static void draw_eye_arch_up(uint8_t *fb, int cx, int cy, int w, int h)
{
    int hw = w / 2, hh = h / 2;
    for (int y = -hh; y <= hh; y++) {
        for (int x = -hw; x <= hw; x++) {
            if (point_in_arch_centered(x, -y, w, h)) {
                set_pixel(fb, cx + x, cy + y);
            }
        }
    }
}

static void draw_filled_circle(uint8_t *fb, int cx, int cy, int r)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                set_pixel(fb, cx + x, cy + y);
            }
        }
    }
}

// Same shape as draw_filled_circle(), but punches it out (dark) instead of
// lighting it -- see clear_pixel()'s comment. Used for die pips.
static void clear_filled_circle(uint8_t *fb, int cx, int cy, int r)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                clear_pixel(fb, cx + x, cy + y);
            }
        }
    }
}

// Thick line: stamps filled circles of radius width_px/2 along the segment,
// sampled finely enough to leave no gaps. Approximates PIL's
// draw.line(..., width=N) / joint="curve" behaviour.
static void draw_thick_line(uint8_t *fb, double x0, double y0, double x1, double y1, int width_px)
{
    double r = width_px / 2.0;
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    int steps = (int)ceil(len * 2.0) + 1;
    int ir = (int)ceil(r);
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / steps;
        double px = x0 + dx * t, py = y0 + dy * t;
        int cx = (int)lround(px), cy = (int)lround(py);
        for (int yy = -ir; yy <= ir; yy++) {
            for (int xx = -ir; xx <= ir; xx++) {
                if (xx * xx + yy * yy <= r * r) {
                    set_pixel(fb, cx + xx, cy + yy);
                }
            }
        }
    }
}

// A big "X" with capped corners, for the ERROR expression's eyes.
static void draw_eye_x(uint8_t *fb, int cx, int cy, int r, int width_px)
{
    draw_thick_line(fb, cx - r, cy - r, cx + r, cy + r, width_px);
    draw_thick_line(fb, cx - r, cy + r, cx + r, cy - r, width_px);
    int cap = width_px / 2;
    static const int dxs[4] = { -1, 1, -1, 1 };
    static const int dys[4] = { -1, -1, 1, 1 };
    for (int i = 0; i < 4; i++) {
        draw_filled_circle(fb, cx + dxs[i] * r, cy + dys[i] * r, cap);
    }
}

// --- Pose model --------------------------------------------------------
//
// Every expression is data -- a left eye and a right eye, each described by
// a shape tag plus the numeric knobs that shape needs -- built once in
// get_face_pose() below instead of drawn directly. This is what lets
// face_display_show() interpolate smoothly between two expressions (blend
// the numeric fields frame-by-frame) instead of hard-cutting, and keeps
// get_face_pose() itself a plain, side-effect-free table any future caller
// (or test) can inspect without a framebuffer.
//
// No mouth: matches the real Anki Cozmo hardware this is modeled after,
// whose face is two eyes on a screen and nothing else -- every mood reads
// entirely from eye shape/position. An earlier version of this component
// drew mouths (arc smiles/frowns, a dot-strip for THINKING, "SETUP" spelled
// out in a tiny bitmap font); removed once real-hardware photos made clear
// how off that was from the reference, taking draw_capped_arc(),
// draw_circle_outline(), and the bitmap-font block with it.
//
// Socket centers (eye_cy, left_x, right_x) are NOT part of the pose --
// they're fixed layout constants (see render_pose()) that every expression
// shares; dx/dy below offset a shape from its socket
// center for expressions that shift eyes around (EXPR_LOOKING_*).

typedef enum { EYE_PILL, EYE_ARCH_DOWN, EYE_ARCH_UP, EYE_CIRCLE, EYE_X } eye_shape_t;

typedef struct {
    eye_shape_t shape;
    double w, h;      // bounding box
    double radius;    // corner radius -- EYE_PILL only
    double tilt_deg;  // EYE_PILL only
    double dx, dy;    // offset from this eye's socket center
} eye_pose_t;

typedef struct {
    eye_pose_t left, right;
} face_pose_t;

// Any two poses with matching shape tags interpolate continuously (all
// numeric fields lerp together); a shape-tag mismatch can't be blended
// geometrically (there's no continuous path from a circle to an "X"), so
// render_pose() below snaps that part to its target the moment t reaches
// 0.5 instead -- see the file header comment on face_display_show() for how
// the two behaviors combine into one animation.

static void get_face_pose(face_expression_t expression, face_pose_t *pose)
{
    // Nominal sizes every case scales from -- NOT socket positions (see the
    // pose-model comment above), just the baseline eye/mouth dimensions.
    // Taller than wide with a modest corner radius (well under half the
    // width) so EYE_PILL reads as a rounded rectangle, not the fully
    // round-ended capsule a radius of min(w,h)/2 would give.
    const double eye_w = WIDTH * 0.15;
    const double eye_h = HEIGHT * 0.42;
    const double eye_radius = eye_w * 0.3;

    eye_pose_t plain_eye = { .shape = EYE_PILL, .w = eye_w, .h = eye_h, .radius = eye_radius };

    // Defaults: EXPR_IDLE's look. Every case below overrides only what it
    // needs to change, the same way the old switch's fallthrough default
    // worked.
    pose->left = plain_eye;
    pose->right = plain_eye;

    switch (expression) {
    case EXPR_SPEAKING_HAPPY:
        // Filled content/happy crescents (Cozmo reference sheet's "Happy"
        // row) -- same EYE_ARCH_UP primitive EXPR_LOOKING_* uses for its
        // arch, mirrored.
        pose->left = pose->right = (eye_pose_t){ .shape = EYE_ARCH_UP, .w = eye_w + 4, .h = eye_h * 0.7 };
        break;

    case EXPR_SPEAKING_SAD:
        pose->left = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w, .h = eye_h, .radius = eye_radius, .tilt_deg = -22 };
        pose->right = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w, .h = eye_h, .radius = eye_radius, .tilt_deg = 22 };
        break;

    case EXPR_SPEAKING_CONFUSED:
        pose->left = plain_eye;
        pose->right = (eye_pose_t){
            .shape = EYE_PILL, .w = eye_w, .h = eye_h, .radius = eye_radius, .tilt_deg = 22, .dy = -eye_h * 0.38,
        };
        break;

    case EXPR_THINKING:
        pose->left = pose->right = (eye_pose_t){
            .shape = EYE_PILL, .w = eye_w, .h = eye_h * 0.6, .radius = eye_h * 0.6 / 3.0, .tilt_deg = -18,
        };
        break;

    case EXPR_ERROR:
        pose->left = pose->right = (eye_pose_t){ .shape = EYE_X, .w = eye_w / 2.0, .h = eye_w / 8.0 < 2 ? 2 : eye_w / 8.0 };
        break;

    case EXPR_SETUP:
        pose->left = pose->right = (eye_pose_t){ .shape = EYE_CIRCLE, .w = eye_h / 5.0 < 2 ? 4 : eye_h / 2.5 };
        break;

    case EXPR_LISTENING:
        pose->left = pose->right = (eye_pose_t){
            .shape = EYE_PILL, .w = eye_w * 1.05, .h = eye_h * 1.25, .radius = eye_radius,
        };
        break;

    case EXPR_SPEAKING_NEUTRAL:
        break; // plain_eye already set as the default above

    case EXPR_BORED:
        pose->left = pose->right = (eye_pose_t){
            .shape = EYE_PILL, .w = eye_w, .h = eye_h * 0.3, .radius = eye_h * 0.3 / 2.0, .dy = 4,
        };
        break;

    case EXPR_LOOKING_LEFT:
    case EXPR_LOOKING_RIGHT: {
        double shift = (expression == EXPR_LOOKING_LEFT) ? -eye_w / 3.0 : eye_w / 3.0;
        pose->left = pose->right = (eye_pose_t){ .shape = EYE_ARCH_DOWN, .w = eye_w, .h = eye_h * 0.75, .dx = shift };
        break;
    }

    // --- New moods, added against the Anki Cozmo reference sheet ---

    case EXPR_ANGRY:
        // Sharp, thin brows angled inward-down toward the nose (mirror of
        // SAD's outward-down droop: signs flipped, flatter/thinner, small
        // radius for a hard edge rather than a soft pill).
        pose->left = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w, .h = eye_h * 0.45, .radius = 2, .tilt_deg = 28 };
        pose->right = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w, .h = eye_h * 0.45, .radius = 2, .tilt_deg = -28 };
        break;

    case EXPR_DISGUSTED:
        // Asymmetric, matching the reference: one eye stays open (flat
        // pill), the other squints into a downward arch, as if recoiling
        // to one side.
        pose->left = plain_eye;
        pose->right = (eye_pose_t){ .shape = EYE_ARCH_DOWN, .w = eye_w, .h = eye_h * 0.4, .dy = eye_h * 0.15 };
        break;

    case EXPR_SURPRISED:
        // Big wide-open circles (reference sheet's "Surprised" row).
        pose->left = pose->right = (eye_pose_t){ .shape = EYE_CIRCLE, .w = eye_h * 0.6 };
        break;

    case EXPR_FEARFUL:
        // Wide-open, pulled-apart eyes: taller than plain, pushed outward
        // via dx.
        pose->left = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w * 0.85, .h = eye_h * 1.15, .radius = eye_radius, .dx = -eye_w * 0.15 };
        pose->right = (eye_pose_t){ .shape = EYE_PILL, .w = eye_w * 0.85, .h = eye_h * 1.15, .radius = eye_radius, .dx = eye_w * 0.15 };
        break;

    case EXPR_IDLE:
    default:
        break; // pose already holds EXPR_IDLE's look (plain_eye)
    }
}

// --- Rendering: turns one face_pose_t into pixels -----------------------

static void render_eye(uint8_t *fb, int socket_cx, int socket_cy, const eye_pose_t *p)
{
    int cx = socket_cx + (int)lround(p->dx);
    int cy = socket_cy + (int)lround(p->dy);
    int w = (int)lround(p->w), h = (int)lround(p->h);
    switch (p->shape) {
    case EYE_PILL:
        draw_eye(fb, cx, cy, w, h, (int)lround(p->radius), p->tilt_deg);
        break;
    case EYE_ARCH_DOWN:
        draw_eye_arch(fb, cx, cy, w, h);
        break;
    case EYE_ARCH_UP:
        draw_eye_arch_up(fb, cx, cy, w, h);
        break;
    case EYE_CIRCLE:
        draw_filled_circle(fb, cx, cy, w > 1 ? w / 2 : 1);
        break;
    case EYE_X:
        draw_eye_x(fb, cx, cy, w, h > 1 ? h : 2);
        break;
    }
}

// Renders `pose` into `framebuffer`, blending toward `target`'s shape at
// t=0.5 for either eye whose shape tag doesn't match `pose`'s own (see the
// pose-model comment above) -- t is where this frame sits in an
// in-progress animation (1.0 for a static, non-animated render).
static void render_pose(const face_pose_t *pose, const face_pose_t *target, double t, uint8_t *framebuffer)
{
    memset(framebuffer, 0, WIDTH * HEIGHT / 8);

    // Layout constants, mirroring face_display.py's render_expression().
    const int eye_cy = (int)(HEIGHT * 0.38);
    const int left_x = (int)(WIDTH * 0.30);
    const int right_x = (int)(WIDTH * 0.70);

    const eye_pose_t *left = (pose->left.shape == target->left.shape || t < 0.5) ? &pose->left : &target->left;
    const eye_pose_t *right = (pose->right.shape == target->right.shape || t < 0.5) ? &pose->right : &target->right;

    render_eye(framebuffer, left_x, eye_cy, left);
    render_eye(framebuffer, right_x, eye_cy, right);
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static eye_pose_t lerp_eye(const eye_pose_t *a, const eye_pose_t *b, double t)
{
    // Only called with a->shape == b->shape (render_pose snaps mismatches
    // itself); shape tag just carries over unchanged.
    return (eye_pose_t){
        .shape = a->shape,
        .w = lerp(a->w, b->w, t), .h = lerp(a->h, b->h, t),
        .radius = lerp(a->radius, b->radius, t), .tilt_deg = lerp(a->tilt_deg, b->tilt_deg, t),
        .dx = lerp(a->dx, b->dx, t), .dy = lerp(a->dy, b->dy, t),
    };
}

// --- Public API ------------------------------------------------------------

esp_err_t face_display_init(i2c_master_bus_handle_t i2c_bus)
{
    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Probe for the display before touching esp_lcd's I2C transactions,
    // which have no timeout: with no physical SSD1306 attached (or ACKing),
    // the very first write in panel_ssd1306_init() blocks forever, hanging
    // main_task until the FreeRTOS task watchdog fires and reboots the
    // board (found on real hardware -- main.c used to skip calling this
    // function entirely as a stopgap). i2c_master_probe() is the
    // documented, bounded-timeout way to check device presence first
    // (driver/i2c_master.h): it sends just the address with a write bit and
    // returns ESP_OK on ACK, ESP_ERR_NOT_FOUND on NACK, or ESP_ERR_TIMEOUT
    // if the bus itself is wedged -- all within xfer_timeout_ms, unlike the
    // unbounded transactions below. This bus is shared with the onboard
    // codecs (see face_display.h) and owned by audio_pipeline -- never
    // deleted here even on failure.
    esp_err_t err = i2c_master_probe(i2c_bus, DISPLAY_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no display found at I2C address 0x%02X (%s) -- skipping display init",
                 DISPLAY_I2C_ADDR, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
        // Required by the newer i2c_master driver (i2c_master_bus_add_device
        // rejects 0 as "invalid scl frequency") -- found on real hardware,
        // not caught by any build since it's a runtime-only default-zero
        // field, not a compile error. 400kHz is SSD1306's standard Fast Mode.
        .scl_speed_hz = 400000,
    };
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_ssd1306_config_t ssd1306_config = { .height = 64 };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    err = esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    ESP_LOGI(TAG, "face_display initialized");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

// Tracks what's currently on screen so the next face_display_show() call
// knows what to animate FROM. s_has_current_pose starts false so the very
// first call (e.g. main.c's post-init EXPR_IDLE) snaps straight to target
// instead of animating in from an undefined pose.
static face_pose_t s_current_pose;
static bool s_has_current_pose;

// Number of intermediate frames pushed between the previous expression and
// the new one, and the delay between each. 8 steps * 25ms = ~200ms total --
// fast enough to read as "fluid" rather than "slow", slow enough that a
// side-glance or a smile visibly moves instead of just appearing.
#define FACE_ANIM_STEPS 8
#define FACE_ANIM_STEP_DELAY_MS 25

esp_err_t face_display_show(face_expression_t expression)
{
    // s_panel is NULL if face_display_init() was never called or never
    // completed successfully (e.g. no physical display wired up) -- no-op
    // rather than dereference a null panel handle through esp_lcd's vtable.
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    face_pose_t target;
    get_face_pose(expression, &target);

    if (!s_has_current_pose) {
        s_current_pose = target;
        s_has_current_pose = true;
        render_pose(&target, &target, 1.0, s_framebuffer);
        return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
    }

    face_pose_t from = s_current_pose;
    esp_err_t err = ESP_OK;
    for (int i = 1; i <= FACE_ANIM_STEPS; i++) {
        double t = (double)i / FACE_ANIM_STEPS;
        face_pose_t interp = {
            .left = lerp_eye(&from.left, &target.left, t),
            .right = lerp_eye(&from.right, &target.right, t),
        };
        render_pose(&interp, &target, t, s_framebuffer);
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
        if (err != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(FACE_ANIM_STEP_DELAY_MS));
    }
    s_current_pose = target;
    return err;
}

// Both face_display_set_gaze_offset() and face_display_set_blink() are
// transient visual nudges on top of whatever expression is showing -- they
// share this last-known-overlay state and re-render together so a blink
// mid-saccade (or vice versa) doesn't clobber the other's effect, matching
// how a real one-shot call from either function looks on screen.
static int s_overlay_dx_px, s_overlay_dy_px;
static float s_overlay_blink = 1.0f; // 1.0 = fully open (default), 0.0 = fully closed

static esp_err_t render_overlay(void)
{
    if (s_panel == NULL || !s_has_current_pose) {
        return ESP_ERR_INVALID_STATE;
    }

    // Deliberately does NOT touch s_current_pose: this overlay is a
    // transient visual nudge on top of whatever's showing, not a change to
    // the base pose face_display_show()'s next animation interpolates from.
    face_pose_t overlay_pose = s_current_pose;
    overlay_pose.left.dx += s_overlay_dx_px;
    overlay_pose.left.dy += s_overlay_dy_px;
    overlay_pose.right.dx += s_overlay_dx_px;
    overlay_pose.right.dy += s_overlay_dy_px;

    if (s_overlay_blink < 1.0f) {
        // Clamp above 0 -- a literal 0px-tall shape degenerates the
        // rounded-rect fill test (w/h ratio blows up), so floor it at a
        // thin sliver instead of a fully collapsed line.
        float openness = s_overlay_blink < 0.05f ? 0.05f : s_overlay_blink;
        overlay_pose.left.h *= openness;
        overlay_pose.right.h *= openness;
    }

    render_pose(&overlay_pose, &overlay_pose, 1.0, s_framebuffer);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

esp_err_t face_display_set_gaze_offset(int dx_px, int dy_px)
{
    s_overlay_dx_px = dx_px;
    s_overlay_dy_px = dy_px;
    return render_overlay();
}

esp_err_t face_display_set_blink(float openness)
{
    s_overlay_blink = openness;
    return render_overlay();
}

// --- Action results (dice roll, coin flip, ...) -----------------------
//
// Deliberately NOT part of the eye_pose_t/face_pose_t system above: a die
// or coin isn't an eye shape, and these are one-off reveals (server sends
// response_end right after, which returns to idle) rather than a mood that
// needs cross-fading. That does mean the transition FROM this screen back
// to idle eyes is a hard cut, not a smooth cross-fade like every other
// expression change -- acceptable for a one-off graphic, not worth
// extending the pose-interpolation system for.

// Standard 1-6 die pip layout, drawn as dark circles punched out of a
// solid white rounded square (see clear_filled_circle()'s comment on why a
// die needs "subtractive" drawing, unlike every other shape in this file).
static void draw_die_face(uint8_t *fb, int value)
{
    memset(fb, 0, WIDTH * HEIGHT / 8);
    int cx = WIDTH / 2, cy = HEIGHT / 2;
    int size = 44;
    draw_rounded_rect(fb, cx, cy, size, size, 8);
    int pip_r = 4;
    int off = size / 4;
    bool center = (value == 1 || value == 3 || value == 5);
    bool corners = (value >= 2);
    bool middles = (value == 4 || value == 5 || value == 6);
    if (center) {
        clear_filled_circle(fb, cx, cy, pip_r);
    }
    if (corners) {
        clear_filled_circle(fb, cx - off, cy - off, pip_r);
        clear_filled_circle(fb, cx + off, cy + off, pip_r);
    }
    if (value >= 4) {
        clear_filled_circle(fb, cx - off, cy + off, pip_r);
        clear_filled_circle(fb, cx + off, cy - off, pip_r);
    }
    if (middles && value == 6) {
        clear_filled_circle(fb, cx - off, cy, pip_r);
        clear_filled_circle(fb, cx + off, cy, pip_r);
    }
}

// "testa" (heads): a plain filled circle. "croce" (tails -- literally
// "cross" in Italian, which is also the coin-flip term for that side): the
// same X shape already used for the ERROR expression's eyes, just scaled
// up. No text/font rendering needed for either.
static void draw_coin_face(uint8_t *fb, bool testa)
{
    memset(fb, 0, WIDTH * HEIGHT / 8);
    int cx = WIDTH / 2, cy = HEIGHT / 2;
    if (testa) {
        draw_filled_circle(fb, cx, cy, 22);
    } else {
        draw_eye_x(fb, cx, cy, 20, 6);
    }
}

#define ACTION_ROLL_STEPS 30      // "rolling"/"flipping" flourish frames before settling -- 30 * 100ms = 3s
#define ACTION_ROLL_STEP_MS 100
#define ACTION_RESULT_HOLD_MS 5000 // how long the settled result stays on screen

esp_err_t face_display_show_action(const char *name, const char *result)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool is_dice = (strcmp(name, "dice_roll") == 0);
    bool is_coin = (strcmp(name, "coin_flip") == 0);
    if (!is_dice && !is_coin) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < ACTION_ROLL_STEPS; i++) {
        if (is_dice) {
            draw_die_face(s_framebuffer, 1 + (int)(esp_random() % 6));
        } else {
            draw_coin_face(s_framebuffer, (i % 2) == 0);
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
        vTaskDelay(pdMS_TO_TICKS(ACTION_ROLL_STEP_MS));
    }

    if (is_dice) {
        int value = atoi(result);
        if (value < 1 || value > 6) {
            value = 1;
        }
        draw_die_face(s_framebuffer, value);
    } else {
        draw_coin_face(s_framebuffer, strcmp(result, "testa") == 0);
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
    vTaskDelay(pdMS_TO_TICKS(ACTION_RESULT_HOLD_MS));
    return err;
}

// --- Music playback (scrolling notes) -----------------------------------
//
// Non-blocking, unlike face_display_show_action() above: a track can play
// for minutes, so this can't hold the calling task hostage the way a
// dice/coin reveal briefly can. main.c's orchestrator_task instead calls
// this periodically (same "small periodic update from the normal loop
// tick" pattern as its blink/saccade overlays) for as long as
// HARO_STATE_PLAYING_MUSIC lasts, advancing scroll_offset a little each
// time -- see main.c's music-notes block.

// A simple stylized eighth note: a filled circle (notehead) + a stem +  a
// short diagonal flag. No font/text rendering needed (matches this file's
// existing no-font-since-the-mouth-was-removed stance).
static void draw_note(uint8_t *fb, int cx, int cy)
{
    draw_filled_circle(fb, cx, cy + 8, 5);
    draw_thick_line(fb, cx + 5, cy + 8, cx + 5, cy - 12, 2);
    draw_thick_line(fb, cx + 5, cy - 12, cx + 12, cy - 6, 2);
}

esp_err_t face_display_set_music_notes(int scroll_offset)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_framebuffer, 0, WIDTH * HEIGHT / 8);
    int cy = HEIGHT / 2;
    const int spacing = 42;
    // Notes are spaced `spacing` px apart, shifted left by scroll_offset
    // (mod spacing so it wraps smoothly forever rather than overflowing),
    // starting one full spacing past the right edge and stepping down past
    // the left edge -- covers the whole width with no visible pop-in/out
    // at the wrap point.
    int shift = ((scroll_offset % spacing) + spacing) % spacing;
    for (int x = WIDTH + spacing - shift; x > -spacing; x -= spacing) {
        draw_note(s_framebuffer, x, cy);
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

// --- Scrolling text (server-unreachable diagnostic; boot-time wake-word
// --- reminder) -----------------------------------------------------------
//
// The only text ever drawn on this display -- see the pose-model comment
// above for why every mood/expression is font-free by design. This is a
// deliberate, scoped exception for a couple of diagnostic/informational
// screens, not a reintroduction of the old mouth/font system: font5x7 is a
// minimal 5x7 glyph set covering only what those specific screens' text
// needs (see font5x7.h), not general text rendering.

#define SCROLLING_TEXT_GLYPH_GAP_PX 1
// Blank gap between one loop of the scrolling string and the next repeat,
// so the marquee doesn't read as the string running directly into itself.
#define SCROLLING_TEXT_LOOP_GAP_PX 20

static void draw_text(uint8_t *fb, const char *text, int x, int y)
{
    int cursor_x = x;
    for (const char *p = text; *p != '\0'; p++) {
        const uint8_t *glyph = font5x7_glyph(*p);
        for (int col = 0; col < FONT5X7_WIDTH; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < FONT5X7_HEIGHT; row++) {
                if (bits & (1u << row)) {
                    set_pixel(fb, cursor_x + col, y + row);
                }
            }
        }
        cursor_x += FONT5X7_WIDTH + SCROLLING_TEXT_GLYPH_GAP_PX;
    }
}

esp_err_t face_display_set_scrolling_text(const char *text, int scroll_offset)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_framebuffer, 0, WIDTH * HEIGHT / 8);
    int len = (int)strlen(text);
    int string_width = len * (FONT5X7_WIDTH + SCROLLING_TEXT_GLYPH_GAP_PX);
    int period = string_width + SCROLLING_TEXT_LOOP_GAP_PX;
    int y = (HEIGHT - FONT5X7_HEIGHT) / 2;

    // Same wrap-forever approach as face_display_set_music_notes() above:
    // shift left by scroll_offset (mod period), drawing repeated copies of
    // the whole string spaced `period` px apart -- starting one full
    // period past the right edge and stepping left past the left edge
    // covers the whole width with no visible pop-in/out at the wrap point.
    int shift = ((scroll_offset % period) + period) % period;
    for (int x = WIDTH + period - shift; x > -string_width; x -= period) {
        draw_text(s_framebuffer, text, x, y);
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

// --- WiFi connection status (boot-time, wifi_provisioning.c) -----------
//
// Two brief, discrete moments in the startup WiFi sequence -- same
// "replaces the eyes entirely, one-off, doesn't touch s_current_pose"
// contract as the action results above, not the eye_pose_t/face_pose_t
// mood system (this isn't a mood, it's boot-time status). The third
// moment, "entered AP/provisioning mode" (SoftAP up, waiting for someone
// to submit new credentials via the app), deliberately does NOT get a
// third icon here -- it reuses the existing EXPR_SETUP expression via the
// normal face_display_show(EXPR_SETUP) instead. That wait is genuinely
// indefinite (until a human acts), which fits the interruptible,
// cross-fading mood system better than a one-shot graphic, and EXPR_SETUP
// (small circle eyes) already existed for exactly this moment.

// A conventional "wifi signal" glyph -- three concentric arcs bulging
// upward over a dot, all centered on the dot -- the same real-world symbol
// every phone status bar uses, so it reads correctly at this display's low
// resolution without needing to be a literal photo of a router.
static void draw_wifi_icon(uint8_t *fb)
{
    memset(fb, 0, WIDTH * HEIGHT / 8);
    int cx = WIDTH / 2, hub_y = 46;
    draw_filled_circle(fb, cx, hub_y, 4);

    // Angle sweep centered on 90 degrees (straight up in this file's screen
    // convention: y = hub_y - r*sin(theta), same rotation sense draw_eye()
    // uses elsewhere) so each arc bulges upward with both ends curving down
    // toward the dot -- 20..160 puts the ends at a shallow, natural-looking
    // angle rather than reaching all the way down to the dot's own height.
    const double start_deg = 20.0, end_deg = 160.0;
    const int radii[3] = { 12, 20, 28 };
    const int steps = 16;
    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < steps; i++) {
            double t0 = (start_deg + (end_deg - start_deg) * i / steps) * M_PI / 180.0;
            double t1 = (start_deg + (end_deg - start_deg) * (i + 1) / steps) * M_PI / 180.0;
            draw_thick_line(fb, cx + radii[a] * cos(t0), hub_y - radii[a] * sin(t0),
                             cx + radii[a] * cos(t1), hub_y - radii[a] * sin(t1), 3);
        }
    }
}

// A chunky thumbs-up: a rounded-rect fist with a narrower rounded-rect
// thumb overlapping its upper-left corner, sticking up -- same bold
// rounded-rect vocabulary draw_die_face()'s die body already uses, kept
// simple since this display has no room (or need) for finer hand detail.
static void draw_thumbs_up_icon(uint8_t *fb)
{
    memset(fb, 0, WIDTH * HEIGHT / 8);
    int cx = WIDTH / 2;
    draw_rounded_rect(fb, cx + 4, 42, 34, 22, 8);  // fist
    draw_rounded_rect(fb, cx - 8, 22, 14, 26, 6);  // thumb
}

esp_err_t face_display_show_wifi_searching(void)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    draw_wifi_icon(s_framebuffer);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

// How long the thumbs-up stays up before returning -- long enough to
// actually register as a confirmation, short enough not to noticeably
// delay the rest of boot. Held here (like face_display_show_action()'s
// own vTaskDelay) so callers don't need to manage this timing themselves.
#define WIFI_CONNECTED_HOLD_MS 1500

esp_err_t face_display_show_wifi_connected(void)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    draw_thumbs_up_icon(s_framebuffer);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
    vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECTED_HOLD_MS));
    return err;
}

// --- Volume level (main.c, in response to the KEY1/KEY3 buttons) --------
//
// Same "replaces the eyes entirely, one-off overlay, doesn't touch
// s_current_pose" contract as the WiFi status icons above -- not a mood,
// so not part of face_display_show()'s cross-fading system. Unlike those
// (shown once, held for a fixed duration via this file's own vTaskDelay),
// this draws once per call and returns immediately -- main.c owns the
// multi-second auto-return-to-normal timing itself (same "caller decides
// when to stop calling/showing something else" contract as face_display_
// set_scrolling_text()/set_music_notes(), just without needing repeated
// calls to animate an otherwise-static image).

#define VOLUME_BAR_SEGMENT_COUNT 10
#define VOLUME_BAR_SEGMENT_W 5
#define VOLUME_BAR_SEGMENT_GAP 2
#define VOLUME_BAR_H 28
#define VOLUME_BAR_X0 46
#define VOLUME_ICON_CX 20

// A speaker glyph -- a small housing (filled square) feeding a cone that
// widens to the right (per-column scanline fill, since draw_rounded_rect()
// only covers plain rectangles), plus two short sound-wave arcs beyond the
// cone's mouth, using the same arc-sweep technique as draw_wifi_icon()
// above (a smaller sweep/radius here since this icon shares the screen
// with the volume bar, unlike WiFi's icon which has the whole display).
static void draw_speaker_icon(uint8_t *fb, int cx, int cy)
{
    draw_rounded_rect(fb, cx - 5, cy, 8, 12, 1); // housing, spans roughly x:[cx-9, cx-1]
    int x0 = cx - 1, x1 = cx + 9;
    double h0 = 6.0, h1 = 12.0; // cone half-height: matches the housing's own half-height, widens to the mouth
    for (int x = x0; x <= x1; x++) {
        double t = (double)(x - x0) / (double)(x1 - x0);
        int half_h = (int)lround(h0 + (h1 - h0) * t);
        for (int y = cy - half_h; y <= cy + half_h; y++) {
            set_pixel(fb, x, y);
        }
    }
    const double start_deg = -40.0, end_deg = 40.0;
    const int radii[2] = { 6, 11 };
    const int steps = 8;
    for (int a = 0; a < 2; a++) {
        for (int i = 0; i < steps; i++) {
            double t0 = (start_deg + (end_deg - start_deg) * i / steps) * M_PI / 180.0;
            double t1 = (start_deg + (end_deg - start_deg) * (i + 1) / steps) * M_PI / 180.0;
            draw_thick_line(fb, cx + 10 + radii[a] * cos(t0), cy - radii[a] * sin(t0),
                             cx + 10 + radii[a] * cos(t1), cy - radii[a] * sin(t1), 2);
        }
    }
}

static void draw_hollow_rect(uint8_t *fb, int x0, int y0, int w, int h)
{
    for (int x = x0; x < x0 + w; x++) {
        set_pixel(fb, x, y0);
        set_pixel(fb, x, y0 + h - 1);
    }
    for (int y = y0; y < y0 + h; y++) {
        set_pixel(fb, x0, y);
        set_pixel(fb, x0 + w - 1, y);
    }
}

// 10 vertical segments left-to-right, `level` of them filled solid, the
// rest drawn as a hollow outline -- a "how many of 10 slots are filled"
// bar, not an equalizer (uniform height, not stepped), so it reads
// unambiguously as a level indicator at a glance.
static void draw_volume_bar(uint8_t *fb, int level)
{
    int y0 = HEIGHT / 2 - VOLUME_BAR_H / 2;
    for (int i = 0; i < VOLUME_BAR_SEGMENT_COUNT; i++) {
        int x = VOLUME_BAR_X0 + i * (VOLUME_BAR_SEGMENT_W + VOLUME_BAR_SEGMENT_GAP);
        if (i < level) {
            for (int dx = 0; dx < VOLUME_BAR_SEGMENT_W; dx++) {
                for (int dy = 0; dy < VOLUME_BAR_H; dy++) {
                    set_pixel(fb, x + dx, y0 + dy);
                }
            }
        } else {
            draw_hollow_rect(fb, x, y0, VOLUME_BAR_SEGMENT_W, VOLUME_BAR_H);
        }
    }
}

// `level` is clamped to [1, VOLUME_BAR_SEGMENT_COUNT] rather than treated
// as a caller error -- main.c's own level state is already kept in that
// range, so this is defense-in-depth, not a documented failure mode.
esp_err_t face_display_set_volume_icon(int level)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level < 1) {
        level = 1;
    } else if (level > VOLUME_BAR_SEGMENT_COUNT) {
        level = VOLUME_BAR_SEGMENT_COUNT;
    }

    memset(s_framebuffer, 0, WIDTH * HEIGHT / 8);
    draw_speaker_icon(s_framebuffer, VOLUME_ICON_CX, HEIGHT / 2);
    draw_volume_bar(s_framebuffer, level);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

// --- KEY2 info screens (main.c) -----------------------------------------
//
// Static multi-line text pages, same "replaces the eyes entirely, one-off
// overlay, doesn't touch s_current_pose" contract as the volume icon above
// and the same draw-once-and-return timing: main.c owns when to redraw
// (the network page's values change) and when to leave.

#define INFO_TEXT_ADVANCE_PX (FONT5X7_WIDTH + SCROLLING_TEXT_GLYPH_GAP_PX)
#define INFO_LINE_SPACING_PX 12
#define SIGNAL_BAR_SEGMENT_COUNT 10
#define SIGNAL_BAR_SEGMENT_W 10
#define SIGNAL_BAR_SEGMENT_GAP 2
#define SIGNAL_BAR_H 8
#define SIGNAL_BAR_Y0 33

static int text_width_px(const char *text)
{
    int len = (int)strlen(text);
    return len > 0 ? len * INFO_TEXT_ADVANCE_PX - SCROLLING_TEXT_GLYPH_GAP_PX : 0;
}

esp_err_t face_display_set_network_info(const char *ssid, int rssi_dbm, int level, int ping_ms)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    bool connected = (ssid != NULL && ssid[0] != '\0');
    if (level < 0) {
        level = 0;
    } else if (level > SIGNAL_BAR_SEGMENT_COUNT) {
        level = SIGNAL_BAR_SEGMENT_COUNT;
    }

    // 21 glyphs fit across 128px; an SSID longer than the space left after
    // "wifi: " just clips at the right edge (set_pixel() bounds-checks).
    char line[48];
    memset(s_framebuffer, 0, WIDTH * HEIGHT / 8);
    snprintf(line, sizeof(line), "wifi: %s", connected ? ssid : "non connesso");
    draw_text(s_framebuffer, line, 0, 0);
    if (connected) {
        snprintf(line, sizeof(line), "segnale: %d db", rssi_dbm);
    } else {
        snprintf(line, sizeof(line), "segnale: --");
    }
    draw_text(s_framebuffer, line, 0, 11);
    if (connected) {
        snprintf(line, sizeof(line), "livello: %d/10", level);
    } else {
        snprintf(line, sizeof(line), "livello: --");
    }
    draw_text(s_framebuffer, line, 0, 22);

    // Same filled-vs-hollow "how many of 10" vocabulary as draw_volume_bar(),
    // laid out horizontally and shorter so it fits between text lines.
    int bar_w = SIGNAL_BAR_SEGMENT_COUNT * (SIGNAL_BAR_SEGMENT_W + SIGNAL_BAR_SEGMENT_GAP) - SIGNAL_BAR_SEGMENT_GAP;
    int bar_x0 = (WIDTH - bar_w) / 2;
    for (int i = 0; i < SIGNAL_BAR_SEGMENT_COUNT; i++) {
        int x = bar_x0 + i * (SIGNAL_BAR_SEGMENT_W + SIGNAL_BAR_SEGMENT_GAP);
        if (connected && i < level) {
            for (int dx = 0; dx < SIGNAL_BAR_SEGMENT_W; dx++) {
                for (int dy = 0; dy < SIGNAL_BAR_H; dy++) {
                    set_pixel(s_framebuffer, x + dx, SIGNAL_BAR_Y0 + dy);
                }
            }
        } else {
            draw_hollow_rect(s_framebuffer, x, SIGNAL_BAR_Y0, SIGNAL_BAR_SEGMENT_W, SIGNAL_BAR_H);
        }
    }

    if (ping_ms >= 0) {
        snprintf(line, sizeof(line), "ping: %d ms", ping_ms);
    } else {
        snprintf(line, sizeof(line), "ping: --");
    }
    draw_text(s_framebuffer, line, 0, 48);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}

esp_err_t face_display_set_text_lines(const char *const *lines, int count)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_framebuffer, 0, WIDTH * HEIGHT / 8);
    int block_h = count > 0 ? (count - 1) * INFO_LINE_SPACING_PX + FONT5X7_HEIGHT : 0;
    int y = (HEIGHT - block_h) / 2;
    for (int i = 0; i < count; i++) {
        draw_text(s_framebuffer, lines[i], (WIDTH - text_width_px(lines[i])) / 2, y);
        y += INFO_LINE_SPACING_PX;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}
