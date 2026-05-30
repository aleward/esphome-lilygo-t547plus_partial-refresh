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

void T547::flush_zone(int input_x, int input_y, int input_w, int input_h) {
  ESP_LOGD(TAG, "Flushing isolated zone: x=%d, y=%d, w=%d, h=%d", input_x, input_y, input_w, input_h);

  // 1. Snap X coordinates to 4-bit byte boundaries
  int min_x = input_x & ~1;
  int max_x = (input_x + input_w - 1) | 1;
  int width = max_x - min_x + 1;
  int height = input_h;
  int min_y = input_y;
  int max_y = input_y + height - 1;

  // 2. Allocate the temporary extraction buffer
  size_t temp_buffer_size = (width * height) / 2;
  uint8_t *temp_buffer = (uint8_t *)heap_caps_malloc(temp_buffer_size, MALLOC_CAP_SPIRAM);

  if (temp_buffer == nullptr) {
    ESP_LOGE(TAG, "Memory allocation failed for flush_zone!");
    return;
  }

  // 3. Extract exactly what we need from the main framebuffer
  int dest_idx = 0;
  int full_width = this->get_width_internal();
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x += 2) {
      int src_idx = (y * full_width + x) / 2;
      temp_buffer[dest_idx++] = this->buffer_[src_idx];
    }
  }

  Rect_t area = {
      .x = min_x,
      .y = min_y,
      .width = width,
      .height = height
  };

  // 4. Power on, clear just this pocket, draw, and power off immediately
  epd_poweron();
  epd_clear_area(area);
  // epd_clear_area(area); // TODO: Remove?
  epd_draw_image(area, temp_buffer, BLACK_ON_WHITE);
  epd_poweroff();

  free(temp_buffer);
}

void T547::force_full_refresh() {
  this->pending_full_refresh_ = true;
  this->update(); // Trigger an immediate update loop
}

void T547::display() {
  uint32_t current_time = millis();
  
  // Logic: 10 mins pass OR manual request
  if (this->pending_full_refresh_ || this->last_full_refresh_ == 0 || (current_time - this->last_full_refresh_ >= FULL_REFRESH_INTERVAL_MS)) {
    ESP_LOGD(TAG, "Performing FORCED/SCHEDULED FULL refresh");
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), this->buffer_);
    epd_poweroff();
    
    this->last_full_refresh_ = current_time;
    this->pending_full_refresh_ = false; // Reset the button
  }
}

}  // namespace T547
}  // namespace esphome

#endif  // USE_ESP32_FRAMEWORK_ARDUINO