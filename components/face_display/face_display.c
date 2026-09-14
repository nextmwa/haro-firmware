#include "face_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
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
