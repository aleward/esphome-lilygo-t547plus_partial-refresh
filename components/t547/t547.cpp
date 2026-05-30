#include "t547.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#ifdef USE_ESP32_FRAMEWORK_ARDUINO

#include <esp32-hal-gpio.h>

namespace esphome {
namespace t547 {

static const char *const TAG = "t574";

void T547::setup() {
  ESP_LOGV(TAG, "Initialize called");
  
  // Log memory before initialization
  ESP_LOGI(TAG, "=== Memory Before Display Init ===");
  ESP_LOGI(TAG, "Free heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  ESP_LOGI(TAG, "Total PSRAM: %d bytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  ESP_LOGI(TAG, "Largest free PSRAM block: %d bytes", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  
  epd_init();
  uint32_t buffer_size = this->get_buffer_length_();
  ESP_LOGI(TAG, "Required buffer size: %d bytes (%.2f KB)", buffer_size, buffer_size / 1024.0);

  if (this->buffer_ != nullptr) {
    free(this->buffer_);  // NOLINT
  }

  this->buffer_ = (uint8_t *) ps_malloc(buffer_size);

  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate buffer for display!");
    ESP_LOGE(TAG, "Free heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGE(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGE(TAG, "Largest free PSRAM block: %d bytes", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    
    // Try regular heap_caps_malloc as fallback
    ESP_LOGW(TAG, "Trying fallback allocation with heap_caps_malloc...");
    this->buffer_ = (uint8_t *) heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (this->buffer_ == nullptr) {
      ESP_LOGE(TAG, "Fallback allocation also failed!");
      this->mark_failed();
      return;
    } else {
      ESP_LOGI(TAG, "Fallback allocation succeeded!");
    }
  }

  memset(this->buffer_, 0xFF, buffer_size);
  ESP_LOGI(TAG, "Buffer allocated successfully at address: %p", this->buffer_);
  ESP_LOGV(TAG, "Initialize complete");
}

float T547::get_setup_priority() const { return setup_priority::PROCESSOR; }
size_t T547::get_buffer_length_() {
    return this->get_width_internal() * this->get_height_internal() / 2;
}

void T547::update() {
  this->do_update_();
  this->display();
}

void HOT T547::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x >= this->get_width_internal() || y >= this->get_height_internal() || x < 0 || y < 0)
    return;
  uint8_t gs = 255 - ((color.red * 2126 / 10000) + (color.green * 7152 / 10000) + (color.blue * 722 / 10000));
  epd_draw_pixel(x, y, gs, this->buffer_);

  // Expand the dirty bounding box
  this->has_dirty_rect_ = true;
  if (x < this->dirty_min_x_) this->dirty_min_x_ = x;
  if (x > this->dirty_max_x_) this->dirty_max_x_ = x;
  if (y < this->dirty_min_y_) this->dirty_min_y_ = y;
  if (y > this->dirty_max_y_) this->dirty_max_y_ = y;
}

void T547::dump_config() {
  LOG_DISPLAY("", "T547", this);
  LOG_UPDATE_INTERVAL(this);
}

void T547::eink_off_() {
  ESP_LOGV(TAG, "Eink off called");
  if (panel_on_ == 0)
    return;
  epd_poweroff();
  panel_on_ = 0;
}

void T547::eink_on_() {
  ESP_LOGV(TAG, "Eink on called");
  if (panel_on_ == 1)
    return;
  epd_poweron();
  panel_on_ = 1;
}

void T547::display() {
  ESP_LOGV(TAG, "Display called");
  uint32_t start_time = millis();

  epd_poweron();

  uint32_t current_time = millis();
  bool force_full_refresh = (this->last_full_refresh_ == 0) || 
                            (current_time - this->last_full_refresh_ >= FULL_REFRESH_INTERVAL_MS);

  // Trigger full refresh if 10 mins have passed, OR if nothing specific changed in YAML
  if (force_full_refresh || !this->has_dirty_rect_) {
    ESP_LOGD(TAG, "Performing FULL display refresh");
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), this->buffer_);
    this->last_full_refresh_ = current_time;
    
    ESP_LOGV(TAG, "Display finished (full) (%ums)", millis() - start_time);
  } else {
    ESP_LOGD(TAG, "Performing PARTIAL refresh. Area: (%d, %d) to (%d, %d)", 
             this->dirty_min_x_, this->dirty_min_y_, this->dirty_max_x_, this->dirty_max_y_);

    // 1. Snap X coordinates to even numbers (4-bit boundary alignment). 
    // Because this is a 4-bit grayscale display, 1 byte = 2 pixels.
    // Snapping to even boundaries ensures we can cleanly copy whole bytes.
    int min_x = this->dirty_min_x_ & ~1; 
    int max_x = this->dirty_max_x_ | 1;  
    
    int width = max_x - min_x + 1;
    int height = this->dirty_max_y_ - this->dirty_min_y_ + 1;

    // 2. Allocate the temporary extraction buffer in PSRAM
    size_t temp_buffer_size = (width * height) / 2;
    uint8_t *temp_buffer = (uint8_t *)heap_caps_malloc(temp_buffer_size, MALLOC_CAP_SPIRAM);

    if (temp_buffer == nullptr) {
      ESP_LOGE(TAG, "Memory allocation failed for partial refresh!");
      epd_poweroff();
      return;
    }

    // 3. Blit (extract) the packed pixels from the main buffer
    int dest_idx = 0;
    int full_width = this->get_width_internal();
    for (int y = this->dirty_min_y_; y <= this->dirty_max_y_; y++) {
      for (int x = min_x; x <= max_x; x += 2) {
        int src_idx = (y * full_width + x) / 2;
        temp_buffer[dest_idx++] = this->buffer_[src_idx];
      }
    }

    // 4. Define the physical rectangle, clear it, and draw the cropped buffer
    Rect_t area = {
        .x = min_x,
        .y = this->dirty_min_y_,
        .width = width,
        .height = height
    };

    epd_clear_area(area);
    epd_draw_image(area, temp_buffer, BLACK_ON_WHITE); 

    free(temp_buffer);
    
    ESP_LOGV(TAG, "Display finished (partial) (%ums)", millis() - start_time);
  }

  epd_poweroff();

  // Reset the dirty tracker for the next loop
  this->has_dirty_rect_ = false;
  this->dirty_min_x_ = 9999;
  this->dirty_min_y_ = 9999;
  this->dirty_max_x_ = -1;
  this->dirty_max_y_ = -1;
}

}  // namespace T547
}  // namespace esphome

#endif  // USE_ESP32_FRAMEWORK_ARDUINO