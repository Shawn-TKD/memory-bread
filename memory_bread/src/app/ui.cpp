#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "ui.h"
#include "draw.h"
#include "notes.h"
#include "battery.h"
#include "rtc.h"
#include "../../logo_bitmap.h"
#include "../../sounds.h"
#include "SD_MMC.h"

#define W   200
#define H   200

// ─── Icons ────────────────────────────────────────────────────────────────

void iconMicWhite(int cx, int cy) {
  fillRect(cx-13, cy-36, 26, 44, WHITE);
  fillCircle(cx, cy-36, 13, WHITE);
  fillCircle(cx, cy+8,  13, WHITE);
  strokeCircle(cx, cy-4, 40, 5, WHITE);
  fillRect(cx-50, cy-50, 100, 50, BLACK);
  fillRect(cx-3,  cy+38, 6,  18, WHITE);
  fillRect(cx-24, cy+54, 48,  5, WHITE);
}

void iconRecordBig(int cx, int cy) {
  fillCircle(cx, cy, 36, WHITE);
  strokeCircle(cx, cy, 52, 5, WHITE);
  strokeCircle(cx, cy, 68, 2, WHITE);
}

void iconCheck(int cx, int cy, bool filled) {
  if (filled) {
    fillCircle(cx, cy, 44, BLACK);
    for (int t=-3;t<=3;t++) {
      line(cx-22, cy-2+t, cx-6, cy+17+t, WHITE);
      line(cx-6,  cy+17+t, cx+30, cy-22+t, WHITE);
    }
  } else {
    strokeCircle(cx, cy, 44, 3, BLACK);
    for (int t=-2;t<=2;t++) {
      line(cx-22, cy-2+t, cx-6, cy+17+t, BLACK);
      line(cx-6,  cy+17+t, cx+30, cy-22+t, BLACK);
    }
  }
}

void iconError(int cx, int cy) {
  strokeCircle(cx, cy, 44, 3, BLACK);
  for (int t=-3;t<=3;t++) {
    line(cx-22, cy-22+t, cx+22, cy+22+t, BLACK);
    line(cx+22, cy-22+t, cx-22, cy+22+t, BLACK);
  }
}

void iconThinking(int cx, int cy) {
  fillCircle(cx-28, cy, 8, BLACK);
  fillCircle(cx,    cy, 8, BLACK);
  fillCircle(cx+28, cy, 8, BLACK);
}

void iconTag(int cx, int cy) {
  const int pts[5][2] = {
    {cx-26, cy+4}, {cx-4, cy-18}, {cx+32, cy-18},
    {cx+32, cy+12}, {cx+4,  cy+36}
  };
  for(int i=0;i<4;i++) thickLine(pts[i][0],pts[i][1],pts[i+1][0],pts[i+1][1],4,BLACK);
  thickLine(pts[4][0],pts[4][1],pts[0][0],pts[0][1],4,BLACK);
  fillCircle(cx-2, cy-4, 5, BLACK);
}

void iconSync(int cx, int cy) {
  // Compact two-arrow sync mark.  The former 40 px radius icon extended to
  // y=122 and collided with the progress bar beginning at y=116.
  strokeCircle(cx, cy, 27, 3, BLACK);
  fillRect(cx+8, cy-34, 20, 16, WHITE);
  thickLine(cx+8,  cy-27, cx+25, cy-27, 2, BLACK);
  thickLine(cx+25, cy-27, cx+18, cy-34, 2, BLACK);
  thickLine(cx+25, cy-27, cx+18, cy-20, 2, BLACK);
  fillRect(cx-28, cy+18, 20, 16, WHITE);
  thickLine(cx-25, cy+27, cx-8,  cy+27, 2, BLACK);
  thickLine(cx-25, cy+27, cx-18, cy+20, 2, BLACK);
  thickLine(cx-25, cy+27, cx-18, cy+34, 2, BLACK);
}

void iconWifi(int cx, int cy) {
  int base = cy + 26;
  strokeCircle(cx, base, 50, 5, BLACK);
  strokeCircle(cx, base, 32, 5, BLACK);
  strokeCircle(cx, base, 14, 5, BLACK);
  fillRect(0, base, W, H - base, WHITE);
  fillCircle(cx, base, 5, BLACK);
}

void iconNoteLines(int cx, int cy) {
  fillRect(cx-32, cy-12, 64, 6, BLACK);
  fillRect(cx-32, cy+2,  64, 6, BLACK);
  fillRect(cx-32, cy+16, 44, 6, BLACK);
}

// ─── Layout helpers ────────────────────────────────────────────────────────

void drawHeader(const char* title, const char* rightInfo) {
  fillRect(0, 0, W, 28, BLACK);
  drawStrC(W/2, 10, title, 1, WHITE);
  if (rightInfo) {
    int rw = textW(rightInfo, 1);
    drawStr(W - 8 - rw, 10, rightInfo, 1, WHITE);
  }
}

void drawHints(const char* recLabel, const char* pwrLabel) {
  hline(0, 179, W, BLACK);
  fillRect(0, 180, W, 20, WHITE);
  drawStr(8, 186, recLabel, 1, BLACK);
  int rw = textW(pwrLabel, 1);
  drawStr(W - 8 - rw, 186, pwrLabel, 1, BLACK);
}

void drawBadge(int cx, int cy, const char* text, bool filled) {
  char up[32]; uppercaseCopy(up, text, sizeof(up));
  int tw = textW(up, 1);
  int bw = tw + 20, bh = 20;
  int bx = cx - bw/2, by = cy - bh/2;
  if (filled) {
    fillRoundRect(bx, by, bw, bh, 9, BLACK);
    drawStrC(cx, by + 6, up, 1, WHITE);
  } else {
    strokeRoundRect(bx, by, bw, bh, 9, 2, BLACK);
    drawStrC(cx, by + 6, up, 1, BLACK);
  }
}

void drawPageDots(int cur, int total) {
  if (total <= 1) return;
  int n = min(total, 7);
  int gap = 16;
  int startX = W/2 - ((n-1)*gap)/2;
  for (int i = 0; i < n; i++) {
    int x = startX + i*gap, y = 168;
    if (i == cur % n) fillCircle(x, y, 5, BLACK);
    else              strokeCircle(x, y, 4, 1, BLACK);
  }
}

void drawChevronRight(int x, int cy, uint8_t c) {
  thickLine(x,   cy-8, x+8, cy,   2, c);
  thickLine(x+8, cy,   x,   cy+8, 2, c);
}

void drawTinyHint(const char* left, const char* right) {
  (void)left; (void)right;
}

void drawKicker(const char* txt, int y) {
  char up[40]; uppercaseCopy(up, txt, sizeof(up));
  drawStrC(W/2, y, up, 1, BLACK);
}

void drawSoftFrame() {
  strokeRoundRect(12, 12, W-24, H-24, 10, 1, BLACK);
}

void drawProductWordmark(int cx, int y, uint8_t color) {
  drawStr(cx - textW("记忆", 2) / 2, y,      "记忆", 2, color);
  drawStr(cx - textW("面包", 1) / 2, y + 36, "面包", 1, color);
}

void drawModernPill(int x, int y, int w, int h, const char* label, bool active) {
  if (active) {
    fillRoundRect(x, y, w, h, h/2, BLACK);
    drawStrInBox(x, y, w, h, label, 1, WHITE);
  } else {
    strokeRoundRect(x, y, w, h, h/2, 1, BLACK);
    drawStrInBox(x, y, w, h, label, 1, BLACK);
  }
}

void drawDotSelector(int cur, int total, int y) {
  int gap = 17, startX = W/2 - ((total-1)*gap)/2;
  for (int i=0; i<total; i++) {
    int x = startX + i*gap;
    if (i == cur) fillCircle(x, y, 4, BLACK);
    else          strokeCircle(x, y, 4, 1, BLACK);
  }
}

void drawCheckSmall(int cx, int cy, uint8_t color) {
  strokeCircle(cx, cy, 13, 1, color);
  thickLine(cx-6, cy, cx-1, cy+5, 2, color);
  thickLine(cx-1, cy+5, cx+8, cy-6, 2, color);
}

void drawMinimalDocIcon(int cx, int cy, uint8_t color) {
  strokeRoundRect(cx-13, cy-16, 26, 32, 3, 2, color);
  hline(cx-7, cy-5, 14, color);
  hline(cx-7, cy+4, 14, color);
  hline(cx-7, cy+13, 9, color);
}

void drawMinimalTagIcon(int cx, int cy, uint8_t color) {
  thickLine(cx-13, cy, cx-2, cy-13, 2, color);
  thickLine(cx-2, cy-13, cx+14, cy-13, 2, color);
  thickLine(cx+14, cy-13, cx+14, cy+2, 2, color);
  thickLine(cx+14, cy+2, cx+2, cy+15, 2, color);
  thickLine(cx+2, cy+15, cx-13, cy, 2, color);
  fillCircle(cx+4, cy-5, 3, color);
}

void drawMinimalCloudIcon(int cx, int cy, uint8_t color) {
  strokeCircle(cx-8, cy+2, 10, 2, color);
  strokeCircle(cx+4, cy-4, 13, 2, color);
  strokeCircle(cx+15, cy+4, 9, 2, color);
  fillRect(cx-22, cy+4, 47, 16, WHITE);
  hline(cx-21, cy+10, 44, color);
}

void drawMenuTile(int x, int y, int w, int h, const char* label, int icon, bool active) {
  if (active) fillRoundRect(x, y, w, h, 12, BLACK);
  else        strokeRoundRect(x, y, w, h, 12, 1, BLACK);
  uint8_t col = active ? WHITE : BLACK;
  int cx = x + w/2;
  fillCircle(cx, y + 17, 4, col);
  drawStrInBox(x + 4, y + 29, w - 8, 18, label, 1, col);
}

static const char* localizedTag(const char* tag) {
  if (!tag) return "";
  if (!strcasecmp(tag, "Note")) return "随记";
  if (!strcasecmp(tag, "Work")) return "工作";
  if (!strcasecmp(tag, "Idea")) return "想法";
  if (!strcasecmp(tag, "Buy")) return "购买";
  if (!strcasecmp(tag, "Private")) return "私密";
  if (!strcasecmp(tag, "Untagged")) return "未分类";
  return tag;
}

void drawNoteCard(int y, int idx, bool active) {
  const int x = 16, w = 168, h = 39;
  if (active) fillRoundRect(x, y, w, h, 8, BLACK);
  else        strokeRoundRect(x, y, w, h, 8, 1, BLACK);
  uint8_t col = active ? WHITE : BLACK;

  char n[8]; snprintf(n, sizeof(n), "#%03d", noteIndex[idx].num);
  String tagLabel = normalizeForDisplay(String(localizedTag(noteIndex[idx].tag)));
  drawStr(x + 10, y + 5, n, 1, col);
  drawStrFit(x + 66, y + 5, 88, tagLabel.c_str(), 1, col);
  String ticker = noteTickerText(idx);
  if (active) tickerScrollActive = textW(ticker.c_str(), 1) > 145;
  drawTickerText(x + 10, y + 22, 145, ticker, active, col);
}

void drawListMenuCard(int y, const char* title, const char* meta, bool active) {
  const int x = 16, w = 168, h = 32;
  if (active) fillRoundRect(x, y, w, h, 8, BLACK);
  else        strokeRoundRect(x, y, w, h, 8, 1, BLACK);
  uint8_t col = active ? WHITE : BLACK;
  drawStrFit(x + 10, y + 8, meta ? 92 : 140, title, 1, col);
  if (meta && strlen(meta) > 0) {
    int mw = min(textW(meta, 1), 56);
    drawStrFit(x + w - 10 - mw, y + 8, 56, meta, 1, col);
  }
}

// ─── Screens ──────────────────────────────────────────────────────────────

static void drawBolt(int x, int y) {          // small ~10x18 lightning bolt, top-left origin
  fillTriangle(x+7, y,    x+1, y+9,  x+6, y+9,  BLACK);
  fillTriangle(x+5, y+8,  x+10, y+8, x+3, y+18, BLACK);
}

void showIdle() {
  clearWhite();
  int  batt     = readBatteryPercent();
  bool charging = isBatteryCharging();
  drawBatteryRing(batt);
  drawProductWordmark(100, 58, BLACK);

  // numeric battery % (+ charging bolt), centered below the wordmark
  char b[8];
  if (batt < 0) snprintf(b, sizeof(b), "--");
  else          snprintf(b, sizeof(b), "%d%%", batt);
  int tw    = textW(b, 1);
  int boltW = charging ? 14 : 0;
  int x     = 100 - (tw + boltW) / 2;
  if (charging) { drawBolt(x, 132); x += boltW; }
  drawStr(x, 144, b, 1, BLACK);
  refresh();
}

void showBatteryLow(int pct) {
  fillRect(0, 0, W, H, BLACK);
  fillRect(95, 48, 10, 50, WHITE);
  fillRect(95, 108, 10, 10, WHITE);
  char buf[8]; snprintf(buf, sizeof(buf), "%d%%", pct);
  drawStrC(100, 132, buf,       2, WHITE);
  drawStrC(100, 160, "电量低", 1, WHITE);
  refresh();
}

static float recCircleR = 24.0f;   // smoothed radius, carried across frames

static void drawRecordingScreen(uint32_t elapsedMs, int level) {
  (void)elapsedMs;
  // Original look: a solid black screen with one centered white circle...
  fillRect(0, 0, W, H, BLACK);
  // ...that pulses with the live mic level (level is 0..152 from record.cpp):
  // quiet ≈ r24, loud ≈ r68.
  float target = 24.0f + (float)level * 44.0f / 152.0f;
  if (target < 24.0f) target = 24.0f;
  if (target > 68.0f) target = 68.0f;
  // Ease toward the target instead of snapping: snap up fast, fall back slowly
  // for a natural "breathing" pulse rather than a jittery jump each frame.
  float a = (target > recCircleR) ? 0.55f : 0.18f;
  recCircleR += (target - recCircleR) * a;
  fillCircle(W / 2, H / 2, (int)(recCircleR + 0.5f), WHITE);
}

void showRecording() {                         // initial frame (synchronous)
  recCircleR = 24.0f;                           // start from the resting size
  drawRecordingScreen(0, 0);
  refresh();
}

// Periodic, non-blocking update during recording — keeps audio capture / SD
// writes flowing while the panel repaints in the background.
void showRecordingLive(uint32_t elapsedMs, int level) {
  if (displayBusy()) return;                   // skip a frame if the panel is still painting
  drawRecordingScreen(elapsedMs, level);
  display->EPD_DisplayPartTrigger();
}

static void drawLongRecordingScreen(uint32_t elapsedMs, uint32_t remainingMinutes) {
  clearWhite();
  drawStrC(100, 22, "长录音", 1, BLACK);
  fillCircle(100, 68, 13, BLACK);

  uint32_t totalSeconds = elapsedMs / 1000UL;
  char elapsed[16];
  snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu:%02lu",
           (unsigned long)(totalSeconds / 3600UL),
           (unsigned long)((totalSeconds / 60UL) % 60UL),
           (unsigned long)(totalSeconds % 60UL));
  drawStrC(100, 99, elapsed, 2, BLACK);

  char remain[32];
  if (elapsedMs == 0 && remainingMinutes == 0) {
    snprintf(remain, sizeof(remain), "正在检查存储卡");
  } else if (remainingMinutes >= 60) {
    snprintf(remain, sizeof(remain), "约剩 %lu 小时 %lu 分",
             (unsigned long)(remainingMinutes / 60),
             (unsigned long)(remainingMinutes % 60));
  } else {
    snprintf(remain, sizeof(remain), "约剩 %lu 分钟", (unsigned long)remainingMinutes);
  }
  drawStrC(100, 145, remain, 1, BLACK);
  drawStrC(100, 172, "短按录音键结束", 1, BLACK);
}

void showLongRecording() {
  drawLongRecordingScreen(0, 0);
  refresh();
}

void showLongRecordingLive(uint32_t elapsedMs, uint32_t remainingMinutes) {
  if (displayBusy()) return;
  drawLongRecordingScreen(elapsedMs, remainingMinutes);
  display->EPD_DisplayPartTrigger();
}

void showSaved(int num) {
  clearWhite();
  drawCheckSmall(100, 46, BLACK);
  drawStrC(100, 76, "已保存", 1, BLACK);
  char b[8]; snprintf(b, sizeof(b), "#%03d", num);
  drawStrC(100, 105, b, 2, BLACK);
  refresh();
}

void showTagSelect(int cursor) {
  clearWhite();
  if (tagCount <= 0) {
    drawKicker("暂无标签", 34);
    drawStrC(100, 100, "请打开配置网页", 1, BLACK);
    refresh();
    return;
  }
  drawKicker("选择标签", 17);
  const int x = 36, w = 128, h = 21, gap = 7;
  int y0 = 40;
  cursor = constrain(cursor, 0, max(tagCount - 1, 0));
  for (int i=0; i<tagCount; i++) {
    int y = y0 + i*(h+gap);
    drawModernPill(x, y, w, h, localizedTag(tags[i]), i == cursor);
  }
  refresh();
}

void showMenu(int cursor) {
  clearWhite();
  drawStr(16, 14, "菜单", 1, BLACK);
  hline(16, 32, W-32, BLACK);
  const int y0 = 39, step = 29, rowH = 25;
  for (int row = 0; row < MENU_COUNT; row++) {
    bool active = row == cursor;
    int y = y0 + row * step;
    if (active) fillRoundRect(16, y, 168, rowH, 7, BLACK);
    else        strokeRoundRect(16, y, 168, rowH, 7, 1, BLACK);
    uint8_t col = active ? WHITE : BLACK;
    drawStrInBox(16, y, 168, rowH, MENU_ITEMS[row], 1, col);
  }
  refresh();
}

void showTagBrowser(int cursor) {
  clearWhite();
  if (tagCount <= 0) {
    drawKicker("标签", 16);
    drawStrC(100, 100, "暂无标签", 1, BLACK);
    refresh();
    return;
  }
  drawKicker("标签", 16);
  fillRoundRect(28, 56, 144, 54, 17, BLACK);
  cursor = constrain(cursor, 0, max(tagCount - 1, 0));
  drawStrInBox(28, 56, 144, 54, localizedTag(tags[cursor]), 2, WHITE);
  int cnt = 0;
  for (int i=0; i<(int)noteIndex.size(); i++)
    if (strcmp(noteIndex[i].tag, tags[cursor])==0) cnt++;
  char cb[20]; snprintf(cb, sizeof(cb), "%d 条笔记", cnt);
  drawStrC(100, 130, cb, 1, BLACK);
  refresh();
}

void showNoteList(int cursor) {
  if (tickerCursor != cursor) {
    tickerCursor = cursor;
    tickerOffset = 0;
    tickerLastMs = millis();
  }
  tickerScrollActive = false;
  clearWhite();
  int count = filteredCount();
  char cb[20]; snprintf(cb, sizeof(cb), "%d 条", count);
  drawStr(16, 14, "笔记", 1, BLACK);
  int cw = textW(cb, 1);
  drawStr(W-16-cw, 14, cb, 1, BLACK);
  if (count <= 0) {
    drawMinimalDocIcon(100, 76, BLACK);
    drawStrC(100, 116, "暂无笔记", 1, BLACK);
    refresh();
    return;
  }
  const int pageSize = 3;
  int pageStart = (cursor / pageSize) * pageSize;
  int activeRow = cursor - pageStart;
  const int y0 = 43, step = 47;
  int shown = min(pageSize, count - pageStart);
  for (int row=0; row<shown; row++) {
    int vis = pageStart + row;
    int idx = noteAtFilteredIndex(vis);
    if (idx >= 0) drawNoteCard(y0 + row*step, idx, row == activeRow);
  }
  refresh();
}

void showNoteDetail(int cursor) {
  clearWhite();
  int idx = noteAtFilteredIndex(cursor);
  if (idx < 0) {
    drawStrC(100, 96, "未找到", 1, BLACK);
    refresh();
    return;
  }
  char n[8]; snprintf(n, sizeof(n), "#%03d", noteIndex[idx].num);
  drawStr(16, 14, n, 1, BLACK);
  String tagLabel = normalizeForDisplay(String(localizedTag(noteIndex[idx].tag)));
  int tw = textW(tagLabel.c_str(), 1);
  drawStrFit(W-16-min(tw, 82), 14, 82, tagLabel.c_str(), 1, BLACK);
  hline(16, 32, W-32, BLACK);

  if (noteIndex[idx].hasText) {
    char txtPath[64];
    snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, noteIndex[idx].num);
    File f = SD_MMC.open(txtPath);
    char text[2048] = {0};
    if (f) { f.read((uint8_t*)text, 2047); f.close(); }
    String bodyText = normalizeForDisplay(String(text));
    const int linesPerPage = 7;
    int skip = detailScrollPage * linesPerPage;
    detailTotalLines = drawWrappedText(18, 48, 164, 18, linesPerPage, bodyText, BLACK, skip);
    int totalPages = (detailTotalLines + linesPerPage - 1) / linesPerPage;
    if (totalPages > 1) {
      char pageLabel[12];
      snprintf(pageLabel, sizeof(pageLabel), "%d/%d", detailScrollPage + 1, totalPages);
      int lw = textW(pageLabel, 1);
      drawStr(W - 8 - lw, 186, pageLabel, 1, BLACK);
      hline(0, 179, W, BLACK);
    }
  } else {
    iconThinking(100, 82);
    drawStrC(100, 122, "尚未转写", 1, BLACK);
  }
  refresh();
}

void showDeleteConfirm(int noteNum) {
  clearWhite();
  fillRect(0, 0, W, 28, BLACK);
  drawStrC(W/2, 10, "删除笔记", 1, WHITE);
  char label[16]; snprintf(label, sizeof(label), "#%03d", noteNum);
  drawStrC(W/2, 52, label, 2, BLACK);
  drawStrC(W/2, 88, "确认删除这条笔记？", 1, BLACK);
  drawStrC(W/2, 108, "录音和文字都会删除", 1, BLACK);
  hline(0, 179, W, BLACK);
  fillRect(0, 180, W, 20, WHITE);
  drawStr(8, 186, "确认", 1, BLACK);
  int rw = textW("取消", 1);
  drawStr(W - 8 - rw, 186, "取消", 1, BLACK);
  refresh();
}

void showCloudSync(int success, int failed, int total) {
  clearWhite();
  drawKicker("上传笔记", 14);
  iconSync(100, 70);
  int barW = 144, barH = 10, barX = 28, barY = 116;
  strokeRoundRect(barX, barY, barW, barH, 5, 1, BLACK);
  if (total > 0) {
    int processed = min(success + failed, total);
    int fill = (processed * (barW - 4)) / max(total, 1);
    if (fill > 0) fillRoundRect(barX+2, barY+2, fill, barH-4, 3, BLACK);
    char current[32];
    snprintf(current, sizeof(current), "正在上传 %d / %d", min(processed + 1, total), total);
    drawStrC(100, 140, current, 1, BLACK);
    char summary[32];
    snprintf(summary, sizeof(summary), "成功 %d  失败 %d", success, failed);
    drawStrC(100, 166, summary, 1, BLACK);
  } else {
    drawStrC(100, 145, "正在准备", 1, BLACK);
  }
  refresh();
}

void showCloudSyncResult(int success, int failed) {
  clearWhite();
  if (failed == 0) {
    drawCheckSmall(100, 56, BLACK);
    drawStrC(100, 88, "上传完成", 1, BLACK);
  } else {
    iconError(100, 56);
    drawStrC(100, 108, "部分上传失败", 1, BLACK);
  }
  char ok[24]; snprintf(ok, sizeof(ok), "成功 %d 条", success);
  char bad[24]; snprintf(bad, sizeof(bad), "失败 %d 条", failed);
  drawStrC(100, 139, ok, 1, BLACK);
  drawStrC(100, 164, bad, 1, BLACK);
  refresh();
}

void showTranscribing(int done, int total) {
  clearWhite();
  drawKicker("正在转写", 20);
  iconThinking(100, 76);
  int barW = 144, barH = 10, barX = 28, barY = 116;
  strokeRoundRect(barX, barY, barW, barH, 5, 1, BLACK);
  if (total > 0) {
    int fill = (done * (barW - 4)) / max(total, 1);
    if (fill > 0) fillRoundRect(barX+2, barY+2, fill, barH-4, 3, BLACK);
    char b[20]; snprintf(b, sizeof(b), "%d / %d", done, total);
    drawStrC(100, 142, b, 1, BLACK);
  } else {
    drawStrC(100, 142, "请稍候", 1, BLACK);
  }
  refresh();
}

void showWifiConnecting(int attempt, int maxA) {
  clearWhite();
  drawKicker("连接网络", 20);
  iconWifi(100, 84);
  int barW = 130, barH = 10, barX = 35, barY = 140;
  strokeRoundRect(barX, barY, barW, barH, 5, 1, BLACK);
  int fill = (attempt * (barW - 4)) / max(maxA, 1);
  if (fill > 0) fillRoundRect(barX+2, barY+2, fill, barH-4, 3, BLACK);
  char b[20]; snprintf(b, sizeof(b), "%d / %d", attempt, maxA);
  drawStrC(100, 164, b, 1, BLACK);
  refresh();
}

void showDone() {
  clearWhite();
  drawCheckSmall(100, 70, BLACK);
  drawStrC(100, 105, "全部完成", 1, BLACK);
  refresh();
}

void showError(const char* msg) {
  clearWhite();
  iconError(100, 70);
  if (msg && strlen(msg) > 0) drawStrC(100, 118, msg, 1, BLACK);
  else drawStrC(100, 118, "发生错误", 1, BLACK);
  refresh();
}

void showStorageError() {
  clearWhite();
  drawKicker("存储卡未就绪", 22);
  iconError(100, 67);
  drawStrC(100, 108, "请关机后重新插卡", 1, BLACK);
  drawStrC(100, 145, "录音键：重试", 1, BLACK);
  drawStrC(100, 169, "电源键：重启", 1, BLACK);
  forceFullRefresh();
}

void showStorageRetrying() {
  clearWhite();
  drawKicker("检查存储卡", 30);
  iconThinking(100, 88);
  drawStrC(100, 139, "正在重新连接", 1, BLACK);
  refresh();
}

void showUltraSleepScreen() {
  clearWhite();
  bool customCover = false;
  const char* coverPath = nullptr;
  if (storageMounted) {
    coverPath = SD_MMC.exists(SLEEP_COVER_FILE) ? SLEEP_COVER_FILE
              : (SD_MMC.exists(SLEEP_COVER_BAK) ? SLEEP_COVER_BAK : nullptr);
  }
  if (coverPath) {
    File cover = SD_MMC.open(coverPath, FILE_READ);
    const size_t expected = (W * H) / 8;
    if (cover && cover.size() == expected) {
      uint8_t* target = display->getBuffer();
      customCover = cover.read(target, expected) == expected;
    }
    if (cover) cover.close();
  }
  if (!customCover) {
    clearWhite();
    drawProductWordmark(100, 70, BLACK);
  }
  forceFullRefresh();   // this image persists through deep sleep; keep it crisp
}

void showPlaybackOverlay() {
  fillRoundRect(75, 145, 50, 34, 11, BLACK);
  fillTriangle(95, 154, 95, 170, 110, 162, WHITE);
  refresh();
}

void showTransferConnecting() {
  clearWhite();
  drawKicker("传输模式", 18);
  iconWifi(100, 82);
  drawStrC(100, 138, "正在连接", 1, BLACK);
  refresh();
}

void showTransferMode(const char* ip) {
  clearWhite();
  drawKicker("传输模式", 16);
  fillRoundRect(26, 48, 148, 58, 16, BLACK);
  drawStrInBox(26, 48, 148, 24, "记忆面包网页", 1, WHITE);
  drawStrInBox(26, 74, 148, 24, "已开启", 1, WHITE);
  drawStrC(100, 124, "请在浏览器打开", 1, BLACK);
  drawStrC(100, 146, ip, 1, BLACK);
  drawStrC(100, 169, "长按录音键退出", 1, BLACK);
  refresh();
}

void showSettings(int cursor) {
  clearWhite();
  drawStr(16, 14, "设置", 1, BLACK);
  hline(16, 32, W-32, BLACK);
  const int y0 = 38, step = 32, boxH = 28;
  for (int row = 0; row < SETTINGS_COUNT; row++) {
    bool active = row == cursor;
    int y = y0 + row * step;
    if (active) fillRoundRect(16, y, 168, boxH, 8, BLACK);
    else        strokeRoundRect(16, y, 168, boxH, 8, 1, BLACK);
    uint8_t col = active ? WHITE : BLACK;
    if (row == 0) {
      drawStr(28, y + 8, "提示音", 1, col);
      const char* theme = palaSoundThemeName(palaSoundTheme());
      drawStr(W - 28 - textW(theme, 1), y + 8, theme, 1, col);
    } else if (row == 1) {
      drawStr(28, y + 8, "传输", 1, col);
    } else if (row == 2) {
      drawStr(28, y + 8, "设备", 1, col);
    } else if (row == 3) {
      drawStr(28, y + 8, "全部删除", 1, col);
    } else {
      drawStr(28, y + 8, "重置", 1, col);
    }
  }
  refresh();
}

void showDeviceInfo() {
  clearWhite();
  drawStr(16, 14, "设备信息", 1, BLACK);
  hline(16, 32, W-32, BLACK);
  drawStr(18, 50, "固件版本", 1, BLACK);
  drawStrFit(18, 68, 160, FIRMWARE_VERSION, 1, BLACK);
  drawStr(18, 94, "开发板", 1, BLACK);
  drawStrFit(18, 112, 160, "ESP32-S3 ePaper 1.54", 1, BLACK);
  char b[24]; snprintf(b, sizeof(b), "%d 条笔记", (int)noteIndex.size());
  drawStr(18, 138, b, 1, BLACK);
  String soundInfo = String("提示音：") + palaSoundThemeName(palaSoundTheme());
  drawStr(18, 160, soundInfo.c_str(), 1, BLACK);
  drawStr(18, 178, rtcUtcIso().length() ? "时钟：已设置" : "时钟：未设置", 1, BLACK);
  refresh();
}

void showResetConfirm() {
  clearWhite();
  drawKicker("恢复出厂设置", 18);
  drawStrC(100, 64,  "清除网络和密钥？", 1, BLACK);
  drawStrC(100, 86,  "笔记会保留", 1, BLACK);
  hline(20, 110, W - 40, BLACK);
  drawStrC(100, 134, "录音键：确认", 1, BLACK);
  drawStrC(100, 156, "电源键：取消", 1, BLACK);
  refresh();
}

void showResetDone() {
  clearWhite();
  drawCheckSmall(100, 70, BLACK);
  drawStrC(100, 110, "重置完成", 1, BLACK);
  drawStrC(100, 132, "正在重启", 1, BLACK);
  forceFullRefresh();
}

void showDeleteAllConfirm(int count, int cursor) {
  clearWhite();
  fillRect(0, 0, W, 26, BLACK);
  drawStrC(W/2, 9, "全部删除", 1, WHITE);
  char label[24]; snprintf(label, sizeof(label), "%d 条笔记", count);
  drawStrC(W/2, 40, label, 2, BLACK);

  const char* opts[2] = { "仅设备", "设备和 GitHub" };
  const int y0 = 76, step = 34, boxH = 28;
  for (int i = 0; i < 2; i++) {
    bool active = (i == cursor);
    int y = y0 + i * step;
    if (active) fillRoundRect(20, y, 160, boxH, 8, BLACK);
    else        strokeRoundRect(20, y, 160, boxH, 8, 1, BLACK);
    drawStrC(W/2, y + 8, opts[i], 1, active ? WHITE : BLACK);
  }

  hline(0, 179, W, BLACK);
  fillRect(0, 180, W, 20, WHITE);
  drawStr(8, 186, "选择", 1, BLACK);
  const char* r = "长按取消";
  drawStr(W - 8 - textW(r, 1), 186, r, 1, BLACK);
  refresh();
}

void showDeleteAllDone(bool alsoVault) {
  clearWhite();
  drawCheckSmall(100, 70, BLACK);
  drawStrC(100, 110, "已全部删除", 1, BLACK);
  if (alsoVault) drawStrC(100, 132, "同步时删除 GitHub", 1, BLACK);
  forceFullRefresh();
}

// ─── Coalesced async redraw ─────────────────────────────────────────────────
// Navigation (cursor moves) just calls requestRedraw() — instant, non-blocking.
// serviceDisplay() (run each loop) repaints the *current* state asynchronously
// whenever the panel is free, so rapid presses coalesce to the final position
// instead of getting eaten by a blocking ~400 ms refresh.
void redrawCurrentScreen() {
  switch (state) {
    case STATE_MENU:        showMenu(menuCursor);         break;
    case STATE_SETTINGS:    showSettings(settingsCursor); break;
    case STATE_NOTE_LIST:   showNoteList(listCursor);     break;
    case STATE_TAG_BROWSER: showTagBrowser(tagCursor);    break;
    case STATE_TAG_SELECT:  showTagSelect(tagCursor);     break;
    case STATE_NOTE_DETAIL: showNoteDetail(listCursor);   break;
    default: break;
  }
}

void serviceDisplay() {
  if (!displayDirty()) return;
  if (displayBusy()) return;          // a previous async update is still painting
  clearDisplayDirty();
  beginBufferDraw();                  // suppress the internal refresh()
  redrawCurrentScreen();              // draw the latest state into the buffer
  endBufferDraw();
  refreshAsyncFromBuffer();           // start the partial update; returns immediately
}
