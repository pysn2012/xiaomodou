#pragma once
#include "esphome.h"

// 魔豆屏幕辅助绘制（移植自 xiaocheng display_helpers.h，适配 160x128）
// 注意：esphome.includes 是逐个拷贝到构建目录的（主 modou.yaml 里已列出本文件）

// 空闲页问候语（每次进入 idle 随机取一条）
static const char *GREETINGS[] = {
    "今天也要加油鸭", "说魔豆唤醒我", "随时待命中", "今天有什么安排",
    "想定个闹钟吗", "我在魔豆里等你"};
static const int GREETINGS_COUNT = sizeof(GREETINGS) / sizeof(GREETINGS[0]);

// 电池图标：Material Design Icons 10 级（10%~100%）
// （魔豆无独立电压传感器，充电图标暂不做——GPIO36 只出百分比）
static const char *BATTERY_ICONS[10] = {
    "\U000F007A", "\U000F007B", "\U000F007C", "\U000F007D", "\U000F007E",
    "\U000F007F", "\U000F0080", "\U000F0081", "\U000F0082", "\U000F0079"};

// 所有页面共用的顶部状态行（y=0，从右往左）：
//   电池（常显，电量数据无效则不画）→ WiFi 图标（连接时）→ HA 图标（API 连接时）
//   左上角：DSP FAIL 优先提示（红色），否则显示 IP
static void draw_top_status(esphome::display::Display &it,
                            esphome::font::Font *icon_font,
                            esphome::font::Font *small_font,
                            esphome::Color color,
                            esphome::sensor::Sensor *percent,
                            esphome::text_sensor::TextSensor *ip,
                            esphome::binary_sensor::BinarySensor *api_status,
                            bool dsp_ok) {
  const int icon_w = 18;
  int x = it.get_width();
  // 电池图标（最右，常显）
  float p = percent->state;
  if (!isnan(p)) {
    int pct = (int) (p + 0.5f);
    if (pct < 1)
      pct = 1;
    if (pct > 100)
      pct = 100;
    x -= icon_w;
    it.printf(x, 0, icon_font, color, esphome::display::TextAlign::TOP_LEFT, "%s",
              BATTERY_ICONS[(pct - 1) / 10]);
  }
  // WiFi 图标（连接时显示）
  if (esphome::wifi::global_wifi_component != nullptr &&
      esphome::wifi::global_wifi_component->is_connected()) {
    x -= icon_w;
    it.printf(x, 0, icon_font, color, esphome::display::TextAlign::TOP_LEFT, "%s", "\U000F05A9");
  }
  // Home Assistant 图标（API 连接时显示）
  if (api_status != nullptr && api_status->state) {
    x -= icon_w;
    it.printf(x, 0, icon_font, color, esphome::display::TextAlign::TOP_LEFT, "%s", "\U000F07B0");
  }
  // 左上角：DSP FAIL 优先（红色），否则 IP
  if (!dsp_ok) {
    it.printf(0, 0, small_font, esphome::Color(255, 80, 80), esphome::display::TextAlign::TOP_LEFT,
              "DSP FAIL");
  } else if (ip->state.length() > 0) {
    it.printf(0, 0, small_font, color, esphome::display::TextAlign::TOP_LEFT, "%s", ip->state.c_str());
  }
}

// 文本自动换行（UTF-8 多字节感知，逐字符累积超宽换行）
static void draw_wrapped(esphome::display::Display &it,
                         const char *text,
                         esphome::font::Font *font,
                         esphome::Color color,
                         int start_x, int start_y, int max_width, int max_lines, int line_height) {
  int y = start_y;
  std::string line;
  std::string str = text;
  for (size_t i = 0; i < str.size() && y < start_y + max_lines * line_height;) {
    uint8_t c = str[i];
    int len =
      (c & 0x80) == 0x00 ? 1 :
      (c & 0xE0) == 0xC0 ? 2 :
      (c & 0xF0) == 0xE0 ? 3 : 4;
    std::string next = line + str.substr(i, len);
    int x1, y1, w, h;
    it.get_text_bounds(start_x, y, next.c_str(), font, esphome::display::TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
    if (w > max_width) {
      it.printf(start_x, y, font, color, esphome::display::TextAlign::TOP_LEFT, "%s", line.c_str());
      y += line_height;
      line.clear();
    } else {
      line = next;
      i += len;
    }
  }
  if (!line.empty() && y < start_y + max_lines * line_height) {
    it.printf(start_x, y, font, color, esphome::display::TextAlign::TOP_LEFT, "%s", line.c_str());
  }
}
