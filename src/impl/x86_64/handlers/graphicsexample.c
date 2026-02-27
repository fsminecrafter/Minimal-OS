#include "graphics.h"
#include "x86_64/gpu.h"
#include "x86_64/pci.h"
#include "logo.h"
#include "serial.h"

// Simple integer sine approximation (returns -100 to 100)
static int32_t simple_sin(int32_t angle) {
    // angle in degrees 0-359
    angle = angle % 360;
    if (angle < 0) angle += 360;
    
    if (angle < 90) {
        return (angle * 100) / 90;
    } else if (angle < 180) {
        return ((180 - angle) * 100) / 90;
    } else if (angle < 270) {
        return -((angle - 180) * 100) / 90;
    } else {
        return -((360 - angle) * 100) / 90;
    }
}

static int32_t simple_cos(int32_t angle) {
    return simple_sin(angle + 90);
}

// Safe, simple graphics demo - no floating point!
void graphics_complete_demo(gpu_device_t* gpu) {
    serial_write_str("\n=== Starting Safe Graphics Showcase ===\n");
    
    // Setup
    graphics_set_gpu(gpu);
    graphics_init();
    graphics_safety_mode = true;  // Use panic
    
    uint32_t width = graphics_get_width();
    uint32_t height = graphics_get_height();
    
    serial_write_str("Screen: ");
    serial_write_dec(width);
    serial_write_str("x");
    serial_write_dec(height);
    serial_write_str("\n");
    
    // Clear to dark blue
    serial_write_str("Step 1: Clear screen\n");
    graphics_clear(0, 0, 32);
    
    // ============================================
    // SECTION 1: BASIC SHAPES
    // ============================================
    serial_write_str("Step 2: Drawing basic shapes\n");
    
    graphics_write_text("BASIC SHAPES", 10, 10, 255, 255, 0);
    
    // Colored pixels
    for (int i = 0; i < 20; i++) {
        graphics_write_pixel(10 + i * 3, 30, 255, 0, 0);
        graphics_write_pixel(10 + i * 3, 32, 0, 255, 0);
        graphics_write_pixel(10 + i * 3, 34, 0, 0, 255);
    }
    
    // Lines - different directions
    graphics_write_line(10, 50, 100, 50, 255, 255, 255);   // Horizontal
    graphics_write_line(110, 50, 110, 100, 255, 0, 0);     // Vertical
    graphics_write_line(120, 50, 180, 100, 0, 255, 0);     // Diagonal
    
    // Rectangles
    graphics_write_rectangle(10, 120, 80, 50, 255, 255, 0);       // Yellow outline
    graphics_fill_rectangle(100, 120, 80, 50, 0, 128, 255);       // Blue filled
    
    serial_write_str("Step 3: Basic shapes done\n");
    
    // ============================================
    // SECTION 2: CIRCLES
    // ============================================
    serial_write_str("Step 4: Drawing circles\n");
    
    graphics_write_text("CIRCLES", 220, 10, 255, 255, 0);
    
    // Circle outlines - different colors
    graphics_write_circle(260, 60, 30, 255, 0, 0);         // Red
    graphics_write_circle(320, 60, 30, 0, 255, 0);         // Green
    graphics_write_circle(380, 60, 30, 0, 0, 255);         // Blue
    
    // Filled circles
    graphics_fill_circle(260, 140, 25, 255, 128, 0);       // Orange
    graphics_fill_circle(320, 140, 25, 128, 0, 255);       // Purple
    
    serial_write_str("Step 5: Circles done\n");
    
    // ============================================
    // SECTION 3: TRIANGLES
    // ============================================
    serial_write_str("Step 6: Drawing triangles\n");
    
    graphics_write_text("TRIANGLES", 440, 10, 255, 255, 0);
    
    // Triangle outline
    graphics_write_triangle(480, 40, 530, 90, 430, 90, 255, 255, 255);
    
    // Filled triangle
    graphics_fill_triangle(480, 110, 530, 160, 430, 160, 255, 128, 0);
    
    serial_write_str("Step 7: Triangles done\n");
    
    // ============================================
    // SECTION 4: TEXT RENDERING
    // ============================================
    serial_write_str("Step 8: Drawing text\n");
    
    graphics_write_text("TEXT COLORS", 10, 200, 255, 255, 0);
    
    // Different colored text
    graphics_write_text("Red text", 10, 220, 255, 0, 0);
    graphics_write_text("Green text", 10, 235, 0, 255, 0);
    graphics_write_text("Blue text", 10, 250, 0, 0, 255);
    graphics_write_text("White text", 10, 265, 255, 255, 255);
    graphics_write_text("Cyan text", 10, 280, 0, 255, 255);
    graphics_write_text("Yellow text", 10, 295, 255, 255, 0);
    
    // Multi-line text
    graphics_write_text("Multi-line\ntext\nworks!", 10, 320, 128, 255, 128);
    
    serial_write_str("Step 9: Text done\n");
    
    // ============================================
    // SECTION 5: PATTERNS
    // ============================================
    serial_write_str("Step 10: Drawing patterns\n");
    
    graphics_write_text("PATTERNS", 220, 200, 255, 255, 0);
    
    // Simple grid (smaller, faster)
    for (int x = 220; x < 420; x += 40) {
        graphics_write_line(x, 220, x, 360, 64, 64, 64);
    }
    for (int y = 220; y < 360; y += 40) {
        graphics_write_line(220, y, 420, y, 64, 64, 64);
    }
    
    // Concentric circles (fewer circles)
    for (uint32_t r = 10; r <= 40; r += 10) {
        uint8_t color = (uint8_t)(r * 6);
        graphics_write_circle(320, 290, r, color, 255 - color, 128);
    }
    
    serial_write_str("Step 11: Patterns done\n");
    
    // ============================================
    // SECTION 6: CHECKERBOARD
    // ============================================
    serial_write_str("Step 12: Drawing checkerboard\n");
    
    graphics_write_text("CHECKERBOARD", 440, 200, 255, 255, 0);
    
    // Small checkerboard (8x8 pattern)
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            if ((x + y) % 2 == 0) {
                graphics_fill_rectangle(440 + x * 15, 220 + y * 15, 15, 15, 255, 255, 255);
            } else {
                graphics_fill_rectangle(440 + x * 15, 220 + y * 15, 15, 15, 0, 0, 0);
            }
        }
    }
    
    serial_write_str("Step 13: Checkerboard done\n");
    
    // ============================================
    // SECTION 7: GRADIENTS (Simple, no loops)
    // ============================================
    serial_write_str("Step 14: Drawing gradients\n");
    
    graphics_write_text("GRADIENTS", 10, 400, 255, 255, 0);
    
    // Horizontal gradient (Red to Green) - fewer lines
    for (int x = 0; x < 150; x += 2) {  // Skip every other line for speed
        uint8_t r = (uint8_t)(255 - (x * 255 / 150));
        uint8_t g = (uint8_t)(x * 255 / 150);
        graphics_write_line(10 + x, 420, 10 + x, 450, r, g, 0);
    }
    
    // Vertical gradient (Blue to Yellow) - fewer lines
    for (int y = 0; y < 80; y += 2) {
        uint8_t b = (uint8_t)(255 - (y * 255 / 80));
        uint8_t rg = (uint8_t)(y * 255 / 80);
        graphics_write_line(10, 460 + y, 160, 460 + y, rg, rg, b);
    }
    
    serial_write_str("Step 15: Gradients done\n");
    
    // ============================================
    // SECTION 8: STAR (Using integer math only)
    // ============================================
    serial_write_str("Step 16: Drawing star\n");
    
    graphics_write_text("STAR", 220, 400, 255, 255, 0);
    
    // Draw a 5-pointed star using integer sine/cosine
    int32_t star_cx = 300;
    int32_t star_cy = 480;
    int32_t star_radius = 40;
    
    for (int i = 0; i < 5; i++) {
        int32_t angle1 = i * 72;        // 72 degrees per point
        int32_t angle2 = (i + 2) * 72;
        
        int32_t x1 = star_cx + (simple_cos(angle1) * star_radius) / 100;
        int32_t y1 = star_cy + (simple_sin(angle1) * star_radius) / 100;
        int32_t x2 = star_cx + (simple_cos(angle2) * star_radius) / 100;
        int32_t y2 = star_cy + (simple_sin(angle2) * star_radius) / 100;
        
        graphics_write_line(x1, y1, x2, y2, 255, 255, 0);
    }
    
    serial_write_str("Step 17: Star done\n");
    
    // ============================================
    // SECTION 9: ELLIPSES
    // ============================================
    serial_write_str("Step 18: Drawing ellipses\n");
    
    graphics_write_text("ELLIPSES", 400, 400, 255, 255, 0);
    
    // Two ellipses - different orientations
    graphics_write_ellipse(460, 450, 50, 30, 255, 0, 0);      // Wide
    graphics_write_ellipse(520, 450, 30, 50, 0, 255, 0);      // Tall
    
    serial_write_str("Step 19: Ellipses done\n");
    
    // ============================================
    // SECTION 10: INFO BOX
    // ============================================
    serial_write_str("Step 20: Drawing info box\n");
    
    graphics_write_text("SYSTEM INFO", 10, 580, 255, 255, 0);
    
    // Border
    graphics_write_rectangle(5, 600, 200, 80, 255, 255, 255);
    
    // Info text
    graphics_write_text("Resolution:", 10, 610, 200, 200, 200);
    graphics_write_text("1080x720", 10, 625, 255, 255, 255);
    graphics_write_text("Colors: 16M", 10, 640, 200, 200, 200);
    graphics_write_text("Font: font8x8_basic", 10, 655, 200, 200, 200);
    
    serial_write_str("Step 21: Info box done\n");
    
    //Logo
    serial_write_str("Step 22: Drawing image\n");
    bmp_image_t logo_data;
    graphics_load_bmp(&logo_data,logo_bmp, logo_bmp_len);
    graphics_draw_image(&logo_data, 800, 175);
    graphics_write_text("BMP IMAGE\n", 670, 35, 255, 255, 0);

    // ============================================
    // FINAL BORDER
    // ============================================
    serial_write_str("Step 23: Drawing border\n");
    
    // Screen border
    graphics_write_rectangle(0, 0, width - 1, height - 1, 128, 128, 255);
    
    // Title bar
    graphics_fill_rectangle(width/2 - 120, 0, 240, 20, 0, 0, 128);
    graphics_write_text("GRAPHICS SHOWCASE", width/2 - 72, 5, 255, 255, 255);
    
    // Footer
    graphics_write_text("MinimalOS Graphics v1.0", width/2 - 96, height - 15, 128, 255, 128);
    
    serial_write_str("\n=== Graphics Showcase Complete! ===\n");
}

// Ultra-simple test - just text and basic shapes
void graphics_minimal_test(gpu_device_t* gpu) {
    serial_write_str("Starting minimal graphics test\n");
    
    graphics_set_gpu(gpu);
    graphics_init();
    graphics_safety_mode = false;
    
    // Black background
    graphics_clear(0, 0, 0);
    
    serial_write_str("Drawing text...\n");
    
    // Draw text with visible background
    graphics_fill_rectangle(95, 95, 210, 80, 64, 64, 64);
    
    graphics_write_text("HELLO WORLD!", 100, 100, 255, 255, 255);
    graphics_write_text("Testing 123", 100, 120, 255, 0, 0);
    graphics_write_text("Graphics OK!", 100, 140, 0, 255, 0);
    graphics_write_text("Success!", 100, 160, 255, 255, 0);
    
    serial_write_str("Drawing shapes...\n");
    
    // Simple shapes
    graphics_fill_circle(400, 150, 40, 255, 0, 0);
    graphics_write_rectangle(500, 110, 80, 80, 0, 255, 0);
    graphics_fill_triangle(650, 110, 700, 190, 600, 190, 0, 0, 255);
    
    // Draw a line grid
    for (int x = 0; x < 800; x += 50) {
        graphics_write_line(x, 0, x, 720, 32, 32, 32);
    }
    for (int y = 0; y < 720; y += 50) {
        graphics_write_line(0, y, 1080, y, 32, 32, 32);
    }
    
    serial_write_str("Minimal test complete!\n");
}