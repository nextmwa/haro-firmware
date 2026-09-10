#include "face_display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>

#define GPIO_DISPLAY_SDA GPIO_NUM_8
#define GPIO_DISPLAY_SCL GPIO_NUM_9
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

// Stroked circle outline (used for LISTENING's mouth: draw.ellipse(...,
// outline=1, width=2)).
static void draw_circle_outline(uint8_t *fb, int cx, int cy, int r, int width_px)
{
    double half_w = width_px / 2.0;
    int bound = r + width_px + 1;
    for (int y = -bound; y <= bound; y++) {
        for (int x = -bound; x <= bound; x++) {
            double d = sqrt((double)(x * x + y * y));
            if (fabs(d - r) <= half_w) {
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

// An arc (over an ellipse inscribed in bbox [x0,y0,x1,y1]) with small filled
// circles ("caps") at both ends -- ports face_display.py's `_capped_arc()`.
// Angle convention matches PIL/face_display.py's: 0 deg = +x axis (3
// o'clock), increasing clockwise as displayed (since y grows downward).
static void draw_capped_arc(uint8_t *fb, int x0, int y0, int x1, int y1, double start_deg, double end_deg, int width_px)
{
    double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
    double rx = (x1 - x0) / 2.0, ry = (y1 - y0) / 2.0;
    double r = width_px / 2.0;
    int ir = (int)ceil(r);
    double span = end_deg - start_deg;
    double avg_r = (fabs(rx) + fabs(ry)) / 2.0;
    double arc_len_est = fabs(span) * M_PI / 180.0 * avg_r;
    int steps = (int)ceil(arc_len_est * 2.0) + 2;
    for (int i = 0; i <= steps; i++) {
        double t = start_deg + span * ((double)i / steps);
        double rad = t * M_PI / 180.0;
        double px = cx + rx * cos(rad), py = cy + ry * sin(rad);
        int pcx = (int)lround(px), pcy = (int)lround(py);
        for (int yy = -ir; yy <= ir; yy++) {
            for (int xx = -ir; xx <= ir; xx++) {
                if (xx * xx + yy * yy <= r * r) {
                    set_pixel(fb, pcx + xx, pcy + yy);
                }
            }
        }
    }
    int cap = width_px / 2;
    if (cap < 1) {
        cap = 1;
    }
    double angs[2] = { start_deg, end_deg };
    for (int k = 0; k < 2; k++) {
        double rad = angs[k] * M_PI / 180.0;
        double px = cx + rx * cos(rad), py = cy + ry * sin(rad);
        draw_filled_circle(fb, (int)lround(px), (int)lround(py), cap);
    }
}

static void draw_eye_smile(uint8_t *fb, int cx, int cy, int w, int h, int width_px)
{
    draw_capped_arc(fb, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 190.0, 350.0, width_px);
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

// --- Minimal built-in 5x7 bitmap font, for SETUP's "SETUP" label ---------
// No font/rendering library is pulled in; this is just the handful of
// glyphs face_display.py's Expression.SETUP label needs.

static const char *glyph_rows(char c, int row)
{
    static const char *const S[7] = { " ### ", "#   #", "#    ", " ### ", "    #", "#   #", " ### " };
    static const char *const E[7] = { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" };
    static const char *const T[7] = { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " };
    static const char *const U[7] = { "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " };
    static const char *const P[7] = { "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " };
    switch (c) {
        case 'S': return S[row];
        case 'E': return E[row];
        case 'T': return T[row];
        case 'U': return U[row];
        case 'P': return P[row];
        default: return "     ";
    }
}

static void draw_char(uint8_t *fb, int x, int y, char c)
{
    for (int row = 0; row < 7; row++) {
        const char *r = glyph_rows(c, row);
        for (int col = 0; col < 5; col++) {
            if (r[col] == '#') {
                set_pixel(fb, x + col, y + row);
            }
        }
    }
}

static void draw_text(uint8_t *fb, int x, int y, const char *text)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        draw_char(fb, cx, y, *p);
        cx += 6; // 5px glyph + 1px spacing
    }
}

// --- Public API ------------------------------------------------------------

void face_display_render(face_expression_t expression, uint8_t *framebuffer)
{
    memset(framebuffer, 0, WIDTH * HEIGHT / 8);

    // Layout constants, mirroring face_display.py's render_expression().
    const int eye_cy = (int)(HEIGHT * 0.38);
    const int eye_w = (int)(WIDTH * 0.20);
    const int eye_h = (int)(HEIGHT * 0.32);
    int eye_radius = (eye_w < eye_h ? eye_w : eye_h) / 2; // pill-shaped ends, Cozmo's "leaf" eye
    const int left_x = (int)(WIDTH * 0.30);
    const int right_x = (int)(WIDTH * 0.70);

    const int mouth_cy = (int)(HEIGHT * 0.78);
    const int mouth_half_w = (int)(WIDTH * 0.18);
    const int mouth_cx = WIDTH / 2;

    switch (expression) {
    case EXPR_SPEAKING_HAPPY:
        draw_eye_smile(framebuffer, left_x, eye_cy, eye_w + 4, eye_h, 3);
        draw_eye_smile(framebuffer, right_x, eye_cy, eye_w + 4, eye_h, 3);
        draw_capped_arc(framebuffer, mouth_cx - mouth_half_w, mouth_cy - 14,
                         mouth_cx + mouth_half_w, mouth_cy + 10, 15.0, 165.0, 3);
        break;

    case EXPR_SPEAKING_SAD:
        draw_eye(framebuffer, left_x, eye_cy, eye_w, eye_h, eye_radius, -22.0);
        draw_eye(framebuffer, right_x, eye_cy, eye_w, eye_h, eye_radius, 22.0);
        draw_capped_arc(framebuffer, mouth_cx - mouth_half_w, mouth_cy,
                         mouth_cx + mouth_half_w, mouth_cy + 22, 200.0, 340.0, 3);
        break;

    case EXPR_SPEAKING_CONFUSED: {
        draw_eye(framebuffer, left_x, eye_cy, eye_w, eye_h, eye_radius, 0.0);
        draw_eye(framebuffer, right_x, (int)(eye_cy * 0.8), eye_w, eye_h, eye_radius, 22.0);
        int third = mouth_half_w / 3;
        double pts[4][2] = {
            { (double)(mouth_cx - mouth_half_w), (double)mouth_cy },
            { (double)(mouth_cx - third), (double)(mouth_cy + 7) },
            { (double)(mouth_cx + third), (double)(mouth_cy - 7) },
            { (double)(mouth_cx + mouth_half_w), (double)mouth_cy },
        };
        for (int i = 0; i < 3; i++) {
            draw_thick_line(framebuffer, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], 3);
        }
        break;
    }

    case EXPR_THINKING: {
        int think_h = (int)(eye_h * 0.6);
        int think_radius = (eye_w < think_h ? eye_w : think_h) / 3;
        draw_eye(framebuffer, left_x, eye_cy, eye_w, think_h, think_radius, -18.0);
        draw_eye(framebuffer, right_x, eye_cy, eye_w, think_h, think_radius, -18.0);
        int dot_r = HEIGHT / 20;
        if (dot_r < 2) {
            dot_r = 2;
        }
        for (int step = -1; step <= 1; step++) {
            int dx = mouth_cx + step * dot_r * 3;
            draw_filled_circle(framebuffer, dx, mouth_cy, dot_r);
        }
        break;
    }

    case EXPR_ERROR: {
        int x_width = eye_w / 8;
        if (x_width < 2) {
            x_width = 2;
        }
        draw_eye_x(framebuffer, left_x, eye_cy, eye_w / 2, x_width);
        draw_eye_x(framebuffer, right_x, eye_cy, eye_w / 2, x_width);
        draw_rounded_rect(framebuffer, mouth_cx, mouth_cy, mouth_half_w, 4, 2);
        break;
    }

    case EXPR_SETUP: {
        int dot_r = eye_h / 5;
        if (dot_r < 2) {
            dot_r = 2;
        }
        draw_filled_circle(framebuffer, left_x, eye_cy, dot_r);
        draw_filled_circle(framebuffer, right_x, eye_cy, dot_r);
        draw_text(framebuffer, mouth_cx - 20, mouth_cy - 6, "SETUP");
        break;
    }

    case EXPR_LISTENING: {
        int w = (int)(eye_w * 1.05), h = (int)(eye_h * 1.25);
        draw_eye(framebuffer, left_x, eye_cy, w, h, eye_radius, 0.0);
        draw_eye(framebuffer, right_x, eye_cy, w, h, eye_radius, 0.0);
        int r = HEIGHT / 16;
        if (r < 2) {
            r = 2;
        }
        draw_circle_outline(framebuffer, mouth_cx, mouth_cy, r, 2);
        break;
    }

    case EXPR_SPEAKING_NEUTRAL: {
        draw_eye(framebuffer, left_x, eye_cy, eye_w, eye_h, eye_radius, 0.0);
        draw_eye(framebuffer, right_x, eye_cy, eye_w, eye_h, eye_radius, 0.0);
        int mouth_r = HEIGHT / 10;
        if (mouth_r < 3) {
            mouth_r = 3;
        }
        draw_rounded_rect(framebuffer, mouth_cx, mouth_cy, mouth_half_w, mouth_r * 2, mouth_r);
        break;
    }

    case EXPR_IDLE:
    default:
        draw_eye(framebuffer, left_x, eye_cy, eye_w, eye_h, eye_radius, 0.0);
        draw_eye(framebuffer, right_x, eye_cy, eye_w, eye_h, eye_radius, 0.0);
        draw_rounded_rect(framebuffer, mouth_cx, mouth_cy, mouth_half_w, 4, 2);
        break;
    }
}

esp_err_t face_display_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_DISPLAY_SDA,
        .scl_io_num = GPIO_DISPLAY_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        return err;
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
    err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io_handle);
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

esp_err_t face_display_show(face_expression_t expression)
{
    // s_panel is NULL if face_display_init() was never called or never
    // completed successfully (e.g. no physical display wired up) -- no-op
    // rather than dereference a null panel handle through esp_lcd's vtable.
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    face_display_render(expression, s_framebuffer);
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_framebuffer);
}
