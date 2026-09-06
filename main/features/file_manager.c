#include "c_vector.h"
#include "config.h"
#include "icon.h"
#include "storage_file.h"
#include "system_features.h"
#include "touch_ui.h"
#include "ui_utils.h"

enum { SdFileManager_COL_BG = FEATURE_BG };
enum { SdFileManager_STATUS_BAR_H = 20 };
enum { SdFileManager_HEADER_H = 54 };

static text_t SdFileManager_ellipsize(text_t s, int maxW, uint8_t font) {
  if (display_text_width(&(tft), (s).data, font) <= maxW) return s;
  text_t out = s;
  while (strlen((out).data) > 0 &&
         display_text_width(&(tft), (text_join_cstr((out).data, "...")).data,
                            font) > maxW) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  return text_join_cstr((out).data, "...");
}

typedef struct SdFileManager_Entry {
  text_t name;
  text_t path;
  bool isDir;
  uint32_t size;
  bool isUp;  // synthetic ".."
} SdFileManager_Entry;
#define SdFileManager_Entry_INITIALIZER \
  {.isDir = false, .size = 0, .isUp = false}
;

typedef uint8_t SdFileManager_Page;
enum {
  SdFileManager_Page_Browser,
  SdFileManager_Page_Info,
  SdFileManager_Page_ConfirmDelete
};

static SdFileManager_Page SdFileManager_page = SdFileManager_Page_Browser;
static c_vector_t SdFileManager_entries = {0};
static text_t SdFileManager_cwd = {.data = "/"};
static int SdFileManager_sel = 0;
static int SdFileManager_listStart = 0;
static bool SdFileManager_uiDirty = true;
static bool SdFileManager_touchActive = false;
static bool SdFileManager_touchDragging = false;
static int SdFileManager_touchStartX = 0;
static int SdFileManager_touchStartY = 0;
static int SdFileManager_touchLastY = 0;
static int SdFileManager_scrollAccumY = 0;

static FeatureUI_Button SdFileManager_browserBtns[4];
static FeatureUI_Button SdFileManager_infoBtns[2];
static FeatureUI_Button SdFileManager_confirmBtns[2];

static text_t SdFileManager_lastErr = {0};

static int SdFileManager_sdFmContentBottom(void) {
  return featureHasTouchNavBar()
             ? (int)touchNavContentBottomY()
             : (int)(display_height(&(tft)) - FeatureUI_FOOTER_H - 2);
}

static void SdFileManager_sdFmRestoreNavChrome(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  invalidateTouchButtonCue();
  maintainTouchNavBar();
}

static char SdFileManager_s_sdFmNavCache[5][12] = {{0}};

static void SdFileManager_sdFmResetNavLabelCache(void) {
  for (int i = 0; i < 5; ++i) {
    SdFileManager_s_sdFmNavCache[i][0] = '\0';
  }
}

static void SdFileManager_sdFmApplyNavLabels(const char* left, const char* down,
                                             const char* center, const char* up,
                                             const char* right) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  const char* src[5] = {left, down, center, up, right};
  bool same = true;
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    if (strcmp(SdFileManager_s_sdFmNavCache[i], s) != 0) {
      same = false;
      break;
    }
  }
  if (same) {
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const char* s = (src[i] && src[i][0]) ? src[i] : "";
    strncpy(SdFileManager_s_sdFmNavCache[i], s,
            sizeof(SdFileManager_s_sdFmNavCache[i]) - 1);
    SdFileManager_s_sdFmNavCache[i][sizeof(SdFileManager_s_sdFmNavCache[i]) -
                                    1] = '\0';
  }
  setTouchNavLabels(left, down, center, up, right);
  redrawTouchButtonBar();
}

static void SdFileManager_sdFmUpdateBrowserNavLabels(void) {
  SdFileManager_sdFmApplyNavLabels("Exit", "Next", "Refresh", "Prev", "Open");
}

static void SdFileManager_sdFmUpdateInfoNavLabels(void) {
  SdFileManager_sdFmApplyNavLabels("Back", NULL, "Delete", NULL, NULL);
}

static void SdFileManager_sdFmUpdateConfirmNavLabels(void) {
  SdFileManager_sdFmApplyNavLabels("Cancel", NULL, "Delete", NULL, NULL);
}

static void SdFileManager_clampSel(void);
static int sd_entry_order(const void* left, const void* right) {
  const SdFileManager_Entry *a = left, *b = right;
  if (a->isDir != b->isDir) return a->isDir ? -1 : 1;
  return strcmp(a->name.data, b->name.data);
}
static bool SdFileManager_reloadDir(text_t path, text_t* errOut);
static void SdFileManager_resetListDrawCache(void);
static void SdFileManager_drawBrowserPage(bool full);
static void SdFileManager_drawConfirmDeletePage(void);
static void SdFileManager_openSelected(void);
static bool SdFileManager_deleteSelected(text_t* errOut);

static void SdFileManager_sdFmHandleNavButtons(void) {
  if (!featureHasTouchNavBar()) {
    return;
  }
  if (SdFileManager_page == SdFileManager_Page_Browser) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      feature_exit_requested = true;
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_UP) &&
        !((SdFileManager_entries).size == 0)) {
      SdFileManager_sel--;
      SdFileManager_clampSel();
      SdFileManager_drawBrowserPage(false);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_DOWN) &&
        !((SdFileManager_entries).size == 0)) {
      SdFileManager_sel++;
      SdFileManager_clampSel();
      SdFileManager_drawBrowserPage(false);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_RIGHT)) {
      SdFileManager_openSelected();
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      SdFileManager_reloadDir(SdFileManager_cwd, NULL);
      SdFileManager_drawBrowserPage(true);
    }
  } else if (SdFileManager_page == SdFileManager_Page_Info) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      SdFileManager_drawBrowserPage(true);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      SdFileManager_drawConfirmDeletePage();
    }
  } else if (SdFileManager_page == SdFileManager_Page_ConfirmDelete) {
    if (isTouchNavButtonPressedEdge(BTN_LEFT)) {
      SdFileManager_drawBrowserPage(true);
      return;
    }
    if (isTouchNavButtonPressedEdge(BTN_SELECT)) {
      text_t err = {0};
      const bool ok = SdFileManager_deleteSelected(&err);
      if (!ok) {
        showNotification("Delete", (err).data);
        sys_sleep_ms(500);
      }
      SdFileManager_reloadDir(SdFileManager_cwd, NULL);
      SdFileManager_drawBrowserPage(true);
    }
  }
}

static storage_file_t SdFileManager_sdOpenCompat(text_t path) {
  storage_file_t f __attribute__((cleanup(storage_file_close))) =
      storage_file_open((path).data, "rb");
  if (storage_file_valid(&(f))) return storage_file_move(&(f));
  if (strlen((path).data) > 1 && (path).data[0] == '/') {
    storage_file_replace(
        &(f), storage_file_open((text_slice(path, 1, SIZE_MAX)).data, "rb"));
  }
  return storage_file_move(&(f));
}

static bool SdFileManager_sdRemoveCompat(text_t path) {
  if (storage_remove((path).data)) return true;
  if (strlen((path).data) > 1 && (path).data[0] == '/')
    return storage_remove((text_slice(path, 1, SIZE_MAX)).data);
  return false;
}

static bool SdFileManager_sdRmdirCompat(text_t path) {
  if (storage_remove((path).data)) return true;
  if (strlen((path).data) > 1 && (path).data[0] == '/')
    return storage_remove((text_slice(path, 1, SIZE_MAX)).data);
  return false;
}

static text_t SdFileManager_normalizePath(text_t path) {
  if (strlen((path).data) == 0) return TEXT("/");
  text_t p = path;
  if ((p).data[0] != '/') p = text_join_cstr("/", (p).data);

  text_t out = {0};

  bool lastSlash = false;
  for (size_t i = 0; i < strlen((p).data); ++i) {
    char c = (p).data[i];
    if (c == '/') {
      if (!lastSlash) text_append_cstr(&(out), (TEXT(c)).data);
      lastSlash = true;
    } else {
      text_append_cstr(&(out), (TEXT(c)).data);
      lastSlash = false;
    }
  }

  while (strlen((out).data) > 1 && text_suffix_cstr((out).data, "/")) {
    text_erase(&(out), strlen((out).data) - 1, SIZE_MAX);
  }
  return out;
}

static void SdFileManager_clampSel(void) {
  if (((SdFileManager_entries).size == 0)) {
    SdFileManager_sel = 0;
    return;
  }
  if (SdFileManager_sel < 0) SdFileManager_sel = 0;
  if (SdFileManager_sel >= (int)(SdFileManager_entries).size)
    SdFileManager_sel = (int)(SdFileManager_entries).size - 1;
}

static void SdFileManager_listLayout(int* top, int* bottom, int* rowH,
                                     int* maxVisible) {
  (*top) = SdFileManager_STATUS_BAR_H + SdFileManager_HEADER_H + 6;
  (*bottom) = SdFileManager_sdFmContentBottom();
  (*rowH) = 22;
  (*maxVisible) = DIV_MAX(1, ((*bottom) - (*top)) / (*rowH));
}

static void SdFileManager_clampListStart(int maxVisible) {
  int maxStart = DIV_MAX(0, (int)(SdFileManager_entries).size - maxVisible);
  if (SdFileManager_listStart < 0) SdFileManager_listStart = 0;
  if (SdFileManager_listStart > maxStart) SdFileManager_listStart = maxStart;
}

static void SdFileManager_ensureSelectionVisible(int maxVisible) {
  if (((SdFileManager_entries).size == 0)) {
    SdFileManager_listStart = 0;
    return;
  }
  if (SdFileManager_sel < SdFileManager_listStart)
    SdFileManager_listStart = SdFileManager_sel;
  if (SdFileManager_sel >= SdFileManager_listStart + maxVisible)
    SdFileManager_listStart = SdFileManager_sel - maxVisible + 1;
  SdFileManager_clampListStart(maxVisible);
}

static text_t SdFileManager_parentPath(text_t p) {
  text_t np = SdFileManager_normalizePath(p);
  if ((strcmp((np).data, "/") == 0)) return TEXT("/");
  int slash = text_rfind_index(np, text_character('/'));
  if (slash <= 0) return TEXT("/");
  return text_slice(np, 0, slash);
}

static bool SdFileManager_reloadDir(text_t path, text_t* errOut) {
  c_vector_free(&(SdFileManager_entries));
  SdFileManager_lastErr = TEXT("");

  if (!isSDCardAvailable()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    SdFileManager_lastErr = TEXT("SD not mounted");
    return false;
  }

  text_t openPath = SdFileManager_normalizePath(path);

  storage_file_t dir __attribute__((cleanup(storage_file_close))) =
      SdFileManager_sdOpenCompat(openPath);
  if (!storage_file_valid(&(dir))) {
    if (errOut) *errOut = TEXT("Open dir failed");
    SdFileManager_lastErr = TEXT("Open dir failed");
    return false;
  }
  if (!((dir).directory != NULL)) {
    storage_file_close(&(dir));
    if (errOut) *errOut = TEXT("Not a directory");
    SdFileManager_lastErr = TEXT("Not a directory");
    return false;
  }

  SdFileManager_cwd = openPath;

  if (!(strcmp((SdFileManager_cwd).data, "/") == 0)) {
    SdFileManager_Entry up = SdFileManager_Entry_INITIALIZER;
    up.name = TEXT("..");
    up.path = SdFileManager_parentPath(SdFileManager_cwd);
    up.isDir = true;
    up.isUp = true;
    C_VECTOR_PUSH_AS(&(SdFileManager_entries), SdFileManager_Entry, up);
  }

  for (;;) {
    storage_file_t f __attribute__((cleanup(storage_file_close))) =
        storage_file_next(&(dir));
    if (!storage_file_valid(&(f))) break;

    SdFileManager_Entry e = SdFileManager_Entry_INITIALIZER;
    {
      text_t raw = TEXT((f).name);
      while (text_suffix_cstr((raw).data, "/"))
        text_erase(&(raw), strlen((raw).data) - 1, SIZE_MAX);
      int slash = text_rfind_index(raw, text_character('/'));
      text_t base = ((slash >= 0) ? text_slice(raw, slash + 1, SIZE_MAX) : raw);
      while (text_prefix_cstr((base).data, "/")) text_erase(&(base), 0, 1);
      while (text_suffix_cstr((base).data, "/"))
        text_erase(&(base), strlen((base).data) - 1, SIZE_MAX);
      e.name = (strlen((base).data) ? base : raw);
      while (text_prefix_cstr((e.name).data, "/")) text_erase(&(e.name), 0, 1);
      while (text_suffix_cstr((e.name).data, "/"))
        text_erase(&(e.name), strlen((e.name).data) - 1, SIZE_MAX);
      if (strlen((e.name).data) == 0) {
        storage_file_close(&(f));
        continue;
      }
    }
    e.isDir = ((f).directory != NULL);
    e.size = e.isDir ? 0 : (uint32_t)storage_file_size(&(f));
    if ((strcmp((SdFileManager_cwd).data, "/") == 0))
      e.path = text_join_cstr("/", (e.name).data);
    else
      e.path = text_join_cstr(
          (text_join_cstr((SdFileManager_cwd).data, "/")).data, (e.name).data);
    e.path = SdFileManager_normalizePath(e.path);

    C_VECTOR_PUSH_AS(&(SdFileManager_entries), SdFileManager_Entry, e);
    storage_file_close(&(f));
    sys_yield_task();
  }
  storage_file_close(&(dir));

  // Sort, keeping ".." first when present.
  int start = ((SdFileManager_entries).size &&
               ((SdFileManager_Entry*)(SdFileManager_entries).data)[0].isUp)
                  ? 1
                  : 0;
  qsort(((SdFileManager_Entry*)(SdFileManager_entries).data) + start,
        (((SdFileManager_Entry*)(SdFileManager_entries).data +
          (SdFileManager_entries).size)) -
            (((SdFileManager_Entry*)(SdFileManager_entries).data) + start),
        sizeof(*(((SdFileManager_Entry*)(SdFileManager_entries).data) + start)),
        sd_entry_order);

  SdFileManager_sel = 0;
  SdFileManager_listStart = 0;
  SdFileManager_uiDirty = true;
  SdFileManager_resetListDrawCache();
  return true;
}

static void SdFileManager_drawHeader(const char* title) {
  const int w = display_width(&(tft));
  const int y0 = SdFileManager_STATUS_BAR_H;
  display_fill_rect(&(tft), 0, y0, w, SdFileManager_HEADER_H, FEATURE_BG);

  display_set_text_font(&(tft), 1);
  display_set_text_size(&(tft), 1);
  display_set_text_color_2(&(tft), UI_ICON, FEATURE_BG);
  display_set_cursor(&(tft), 8, y0 + 4);
  display_print(&(tft), (TEXT(title)).data);

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  text_t pathLine = SdFileManager_ellipsize(SdFileManager_cwd, w - 16, 1);
  display_set_cursor(&(tft), 8, y0 + 24);
  display_print(&(tft), (pathLine).data);

  text_t right =
      (((SdFileManager_entries).size == 0)
           ? TEXT("0/0")
           : text_join_cstr(
                 (text_join_cstr((TEXT(SdFileManager_sel + 1)).data, "/")).data,
                 (TEXT((int)(SdFileManager_entries).size)).data));
  text_t left =
      text_join_cstr("Items: ", (TEXT((int)(SdFileManager_entries).size)).data);
  int rightW = display_text_width(&(tft), (right).data, 1);
  display_set_cursor(&(tft), 8, y0 + 40);
  display_print(&(tft), (left).data);
  display_set_cursor(&(tft), w - 8 - rightW, y0 + 40);
  display_print(&(tft), (right).data);

  display_draw_line(&(tft), 0, y0 + SdFileManager_HEADER_H, w,
                    y0 + SdFileManager_HEADER_H, UI_LINE);
}

static void SdFileManager_drawHeaderSelCounter(void) {
  const int w = display_width(&(tft));
  const int y0 = SdFileManager_STATUS_BAR_H;
  text_t right =
      (((SdFileManager_entries).size == 0)
           ? TEXT("0/0")
           : text_join_cstr(
                 (text_join_cstr((TEXT(SdFileManager_sel + 1)).data, "/")).data,
                 (TEXT((int)(SdFileManager_entries).size)).data));
  int rightW = display_text_width(&(tft), (right).data, 1);
  display_fill_rect(&(tft), w - 8 - DIV_MAX(rightW, 40), y0 + 38,
                    DIV_MAX(rightW, 40) + 4, 14, FEATURE_BG);
  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, FEATURE_BG);
  display_set_cursor(&(tft), w - 8 - rightW, y0 + 40);
  display_print(&(tft), (right).data);
}

static void SdFileManager_drawListRow(int idx, int y, int rowH, bool selected) {
  enum { x = 8 };
  const int w = display_width(&(tft)) - 16;
  uint16_t bg = selected ? UI_FG : FEATURE_BG;
  uint16_t fg = selected ? UI_ICON : UI_TEXT;

  display_fill_rect(&(tft), x, y, w, rowH - 1, bg);
  display_draw_line(&(tft), x, y + rowH - 1, x + w, y + rowH - 1, UI_LINE);

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), fg, bg);
  text_t label = ((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].name;
  if (((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].isDir &&
      !((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].isUp)
    label = text_join_cstr((label).data, "/");
  label = SdFileManager_ellipsize(label, w - 80, 1);
  display_set_cursor(&(tft), x + 4, y + 6);
  display_print(&(tft), (label).data);

  text_t right = TEXT(
      ((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].isDir ? "DIR"
                                                                      : "");
  if (!((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].isDir &&
      !((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].isUp) {
    char sbuf[16];
    uint32_t kb =
        (((SdFileManager_Entry*)(SdFileManager_entries).data)[idx].size +
         1023) /
        1024;
    snprintf(sbuf, sizeof(sbuf), "%lukB", (unsigned long)kb);
    right = TEXT(sbuf);
  }
  if (strlen((right).data)) {
    int tw = display_text_width(&(tft), (right).data, 1);
    display_set_cursor(&(tft), x + w - tw - 6, y + 6);
    display_print(&(tft), (right).data);
  }
}

static void SdFileManager_drawListScrollbar(int top, int bottom, int start,
                                            int maxVisible) {
  if ((int)(SdFileManager_entries).size <= maxVisible) {
    return;
  }
  int scrollBarX = display_width(&(tft)) - 6;
  int scrollBarHeight = bottom - top - 6;
  int scrollBarY = top + 3;
  int indicatorHeight =
      (maxVisible * scrollBarHeight) / (int)(SdFileManager_entries).size;
  if (indicatorHeight < 6) indicatorHeight = 6;
  int indicatorY = scrollBarY;
  indicatorY = scrollBarY +
               (start * (scrollBarHeight - indicatorHeight)) /
                   DIV_MAX(1, (int)(SdFileManager_entries).size - maxVisible);
  display_fill_rect(&(tft), scrollBarX, scrollBarY, 3, scrollBarHeight,
                    UI_LINE);
  display_fill_rect(&(tft), scrollBarX, indicatorY, 3, indicatorHeight,
                    UI_ACCENT);
}

static int SdFileManager_s_lastListSel = -1;
static int SdFileManager_s_lastListStart = -1;

static void SdFileManager_resetListDrawCache(void) {
  SdFileManager_s_lastListSel = -1;
  SdFileManager_s_lastListStart = -1;
}

static void SdFileManager_drawList(bool full) {
  int top, bottom, rowH, maxVisible;
  SdFileManager_listLayout(&(top), &(bottom), &(rowH), &(maxVisible));

  if (!isSDCardAvailable()) {
    display_fill_rect(&(tft), 0, top, display_width(&(tft)), bottom - top,
                      SdFileManager_COL_BG);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_WARN, SdFileManager_COL_BG);
    display_draw_centre_string(&(tft), "SD not mounted",
                               display_width(&(tft)) / 2, top + 30, 1);
    SdFileManager_resetListDrawCache();
    return;
  }

  if (((SdFileManager_entries).size == 0)) {
    display_fill_rect(&(tft), 0, top, display_width(&(tft)), bottom - top,
                      SdFileManager_COL_BG);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, SdFileManager_COL_BG);
    display_draw_centre_string(&(tft), "Empty folder",
                               display_width(&(tft)) / 2, top + 30, 1);
    SdFileManager_resetListDrawCache();
    return;
  }

  SdFileManager_clampSel();
  SdFileManager_ensureSelectionVisible(maxVisible);

  int start = SdFileManager_listStart;
  int end = DIV_MIN((int)(SdFileManager_entries).size, start + maxVisible);

  const bool windowChanged = full || SdFileManager_s_lastListSel < 0 ||
                             SdFileManager_s_lastListStart != start;
  if (windowChanged) {
    display_fill_rect(&(tft), 0, top, display_width(&(tft)), bottom - top,
                      SdFileManager_COL_BG);
    for (int i = start; i < end; ++i) {
      int y = top + (i - start) * rowH;
      SdFileManager_drawListRow(i, y, rowH, i == SdFileManager_sel);
    }
    SdFileManager_drawListScrollbar(top, bottom, start, maxVisible);
    SdFileManager_drawHeaderSelCounter();
    SdFileManager_s_lastListSel = SdFileManager_sel;
    SdFileManager_s_lastListStart = start;
    return;
  }

  if (SdFileManager_s_lastListSel != SdFileManager_sel) {
    SdFileManager_drawHeaderSelCounter();
    const int oldSel = SdFileManager_s_lastListSel;
    const int newSel = SdFileManager_sel;
    if (oldSel >= start && oldSel < end) {
      SdFileManager_drawListRow(oldSel, top + (oldSel - start) * rowH, rowH,
                                false);
    }
    if (newSel >= start && newSel < end) {
      SdFileManager_drawListRow(newSel, top + (newSel - start) * rowH, rowH,
                                true);
    }
    SdFileManager_s_lastListSel = SdFileManager_sel;
  }
}

static void SdFileManager_drawBrowserFooter(void) {
  if (featureHasTouchNavBar()) {
    SdFileManager_sdFmUpdateBrowserNavLabels();
    return;
  }
  bool sdOk = isSDCardAvailable();
  bool hasSel = !((SdFileManager_entries).size == 0);
  bool selIsUp = false;
  if (hasSel) {
    SdFileManager_clampSel();
    selIsUp =
        ((SdFileManager_Entry*)(SdFileManager_entries).data)[SdFileManager_sel]
            .isUp;
  }

  FeatureUI_drawFooterBg();
  FeatureUI_layoutFooter4(
      &(SdFileManager_browserBtns), "Exit", FeatureUI_ButtonStyle_Secondary,
      "Open", FeatureUI_ButtonStyle_Primary, "Delete",
      FeatureUI_ButtonStyle_Danger, "Refresh", FeatureUI_ButtonStyle_Secondary,
      false, (!sdOk || !hasSel), (!sdOk || !hasSel || selIsUp), false);
  {
    for (size_t c_range_index = 0;
         c_range_index < DIV_ARRAY_SIZE(SdFileManager_browserBtns);
         ++c_range_index) {
      __auto_type b = &(SdFileManager_browserBtns[c_range_index]);
      FeatureUI_drawButton(&((*b)), false);
    }
  }
}

static void SdFileManager_drawBrowserPage(bool full) {
  SdFileManager_page = SdFileManager_Page_Browser;
  if (full) {
    featureClearContent(SdFileManager_COL_BG);
    currentBatteryVoltage = readBatteryVoltage();
    drawStatusBar(currentBatteryVoltage, true, false);
    SdFileManager_drawHeader("SD File Manager");
    SdFileManager_drawBrowserFooter();
    SdFileManager_drawList(true);
    SdFileManager_sdFmRestoreNavChrome();
    SdFileManager_uiDirty = false;
    return;
  }

  SdFileManager_drawList(false);
  SdFileManager_uiDirty = false;
}

static void SdFileManager_drawInfoPage(void) {
  SdFileManager_page = SdFileManager_Page_Info;
  featureClearContent(SdFileManager_COL_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  SdFileManager_drawHeader("File info");

  if (featureHasTouchNavBar()) {
    SdFileManager_sdFmUpdateInfoNavLabels();
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_layoutFooter2(
        &(SdFileManager_infoBtns), "Back", FeatureUI_ButtonStyle_Secondary,
        "Delete", FeatureUI_ButtonStyle_Danger, false,
        (((SdFileManager_entries).size == 0) || !isSDCardAvailable()));
    {
      for (size_t c_range_index = 0;
           c_range_index < DIV_ARRAY_SIZE(SdFileManager_infoBtns);
           ++c_range_index) {
        __auto_type b = &(SdFileManager_infoBtns[c_range_index]);
        FeatureUI_drawButton(&((*b)), false);
      }
    }
  }

  const int top = SdFileManager_STATUS_BAR_H + SdFileManager_HEADER_H + 6;
  const int bottom = SdFileManager_sdFmContentBottom();
  display_fill_rect(&(tft), 0, top, display_width(&(tft)), bottom - top,
                    SdFileManager_COL_BG);

  if (((SdFileManager_entries).size == 0)) {
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, SdFileManager_COL_BG);
    display_draw_centre_string(&(tft), "No selection",
                               display_width(&(tft)) / 2, top + 24, 1);
    SdFileManager_uiDirty = false;
    return;
  }

  SdFileManager_clampSel();
  __auto_type e = &(
      ((SdFileManager_Entry*)(SdFileManager_entries).data)[SdFileManager_sel]);

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_TEXT, SdFileManager_COL_BG);
  display_set_cursor(&(tft), 8, top + 10);
  display_print(&(tft), "Name:");
  display_set_cursor(&(tft), 70, top + 10);
  display_print(&(tft), ((*e).name).data);

  display_set_cursor(&(tft), 8, top + 34);
  display_print(&(tft), "Type:");
  display_set_cursor(&(tft), 70, top + 34);
  display_print(&(tft), (TEXT((*e).isDir ? "Folder" : "File")).data);

  if (!(*e).isDir) {
    display_set_cursor(&(tft), 8, top + 58);
    display_print(&(tft), "Size:");
    display_set_cursor(&(tft), 70, top + 58);
    display_print(&(tft), (text_join_cstr((TEXT((*e).size)).data, " B")).data);
  }

  display_set_text_font(&(tft), 1);
  display_set_cursor(&(tft), 8, top + 88);
  display_print(&(tft), "Path:");
  display_set_cursor(&(tft), 8, top + 102);
  display_print(&(tft), ((*e).path).data);

  SdFileManager_sdFmRestoreNavChrome();
  SdFileManager_uiDirty = false;
}

static void SdFileManager_drawConfirmDeletePage(void) {
  SdFileManager_page = SdFileManager_Page_ConfirmDelete;
  featureClearContent(SdFileManager_COL_BG);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  SdFileManager_drawHeader("Confirm delete");

  if (featureHasTouchNavBar()) {
    SdFileManager_sdFmUpdateConfirmNavLabels();
  } else {
    FeatureUI_drawFooterBg();
    FeatureUI_layoutFooter2(&(SdFileManager_confirmBtns), "Cancel",
                            FeatureUI_ButtonStyle_Secondary, "Delete",
                            FeatureUI_ButtonStyle_Danger, false, false);
    {
      for (size_t c_range_index = 0;
           c_range_index < DIV_ARRAY_SIZE(SdFileManager_confirmBtns);
           ++c_range_index) {
        __auto_type b = &(SdFileManager_confirmBtns[c_range_index]);
        FeatureUI_drawButton(&((*b)), false);
      }
    }
  }

  const int top = SdFileManager_STATUS_BAR_H + SdFileManager_HEADER_H + 6;
  const int bottom = SdFileManager_sdFmContentBottom();
  display_fill_rect(&(tft), 0, top, display_width(&(tft)), bottom - top,
                    SdFileManager_COL_BG);

  display_set_text_font(&(tft), 1);
  display_set_text_color_2(&(tft), UI_WARN, SdFileManager_COL_BG);
  display_draw_centre_string(&(tft), "Delete selected item?",
                             display_width(&(tft)) / 2, top + 18, 1);

  if (!((SdFileManager_entries).size == 0)) {
    __auto_type e = &((
        (SdFileManager_Entry*)(SdFileManager_entries).data)[SdFileManager_sel]);
    display_set_text_font(&(tft), 1);
    display_set_text_color_2(&(tft), UI_TEXT, SdFileManager_COL_BG);
    display_draw_centre_string(&(tft), ((*e).name).data,
                               display_width(&(tft)) / 2, top + 48, 1);
  }

  SdFileManager_sdFmRestoreNavChrome();
  SdFileManager_uiDirty = false;
}

static bool SdFileManager_deleteSelected(text_t* errOut) {
  if (((SdFileManager_entries).size == 0)) {
    if (errOut) *errOut = TEXT("Nothing selected");
    return false;
  }
  SdFileManager_clampSel();
  const SdFileManager_Entry e =
      ((SdFileManager_Entry*)(SdFileManager_entries).data)[SdFileManager_sel];
  if (e.isUp) {
    if (errOut) *errOut = TEXT("Cannot delete ..");
    return false;
  }

  if (!isSDCardAvailable()) {
    if (errOut) *errOut = TEXT("SD not mounted");
    return false;
  }

  bool ok = false;
  if (e.isDir) {
    // ESP32 FS supports rmdir. If directory is not empty it will fail.
    ok = SdFileManager_sdRmdirCompat(e.path);
  } else {
    ok = SdFileManager_sdRemoveCompat(e.path);
  }
  if (!ok) {
    if (errOut) *errOut = TEXT("Delete failed");
    return false;
  }
  return true;
}

static void SdFileManager_openSelected(void) {
  if (((SdFileManager_entries).size == 0)) return;
  SdFileManager_clampSel();
  __auto_type e = &(
      ((SdFileManager_Entry*)(SdFileManager_entries).data)[SdFileManager_sel]);

  if ((*e).isDir) {
    text_t err = {0};
    if (!SdFileManager_reloadDir(SdFileManager_normalizePath((*e).path),
                                 &err)) {
      SdFileManager_drawBrowserPage(true);
      text_t msg = (strlen((err).data) ? err : TEXT("Failed"));
      text_append_cstr(&(msg), "\n");
      text_append_cstr(&(msg), ((*e).path).data);
      showNotification("Open", (msg).data);
      sys_sleep_ms(600);
      return;
    }
    SdFileManager_drawBrowserPage(true);
    return;
  }
  SdFileManager_drawInfoPage();
}

static bool SdFileManager_hitListRow(int x, int y, int* outIdx) {
  int top, bottom, rowH, maxVisible;
  SdFileManager_listLayout(&(top), &(bottom), &(rowH), &(maxVisible));
  if (y < top || y >= bottom) return false;

  SdFileManager_ensureSelectionVisible(maxVisible);
  int start = SdFileManager_listStart;
  int end = DIV_MIN((int)(SdFileManager_entries).size, start + maxVisible);

  int row = (y - top) / rowH;
  int idx = start + row;
  if (idx < start || idx >= end) return false;
  (*outIdx) = idx;
  return true;
}

static void SdFileManager_handleTap(int x, int y) {
  if (SdFileManager_page == SdFileManager_Page_Browser) {
    int idx = -1;
    if (SdFileManager_hitListRow(x, y, &(idx))) {
      if (idx != SdFileManager_sel) {
        SdFileManager_sel = idx;
        SdFileManager_clampSel();
        SdFileManager_drawBrowserPage(false);
      } else {
        SdFileManager_openSelected();
      }
      sys_sleep_ms(140);
      return;
    }

    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI_hit(SdFileManager_browserBtns, 4, x, y);
    }
    if (f >= 0) {
      FeatureUI_drawButton(&(SdFileManager_browserBtns[f]), true);
      sys_sleep_ms(90);
      FeatureUI_drawButton(&(SdFileManager_browserBtns[f]), false);
      sys_sleep_ms(90);

      if (f == 0) {
        feature_exit_requested = true;
        return;
      }  // Exit
      if (f == 1) {
        SdFileManager_openSelected();
        sys_sleep_ms(120);
        return;
      }  // Open
      if (f == 2) {  // Delete
        if (!((SdFileManager_entries).size == 0) && isSDCardAvailable() &&
            !((SdFileManager_Entry*)(SdFileManager_entries)
                  .data)[SdFileManager_sel]
                 .isUp) {
          SdFileManager_drawConfirmDeletePage();
        }
        sys_sleep_ms(120);
        return;
      }
      if (f == 3) {
        SdFileManager_reloadDir(SdFileManager_cwd, NULL);
        SdFileManager_drawBrowserPage(true);
        sys_sleep_ms(120);
        return;
      }  // Refresh
    }
  } else if (SdFileManager_page == SdFileManager_Page_Info) {
    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI_hit(SdFileManager_infoBtns, 2, x, y);
    }
    if (f >= 0) {
      FeatureUI_drawButton(&(SdFileManager_infoBtns[f]), true);
      sys_sleep_ms(90);
      FeatureUI_drawButton(&(SdFileManager_infoBtns[f]), false);
      sys_sleep_ms(90);

      if (f == 0) {
        SdFileManager_drawBrowserPage(true);
        sys_sleep_ms(120);
        return;
      }  // Back
      if (f == 1) {
        SdFileManager_drawConfirmDeletePage();
        sys_sleep_ms(120);
        return;
      }  // Delete
    }
  } else if (SdFileManager_page == SdFileManager_Page_ConfirmDelete) {
    int f = -1;
    if (!featureHasTouchNavBar()) {
      f = FeatureUI_hit(SdFileManager_confirmBtns, 2, x, y);
    }
    if (f >= 0) {
      FeatureUI_drawButton(&(SdFileManager_confirmBtns[f]), true);
      sys_sleep_ms(90);
      FeatureUI_drawButton(&(SdFileManager_confirmBtns[f]), false);
      sys_sleep_ms(90);

      if (f == 0) {  // Cancel
        SdFileManager_drawBrowserPage(true);
        sys_sleep_ms(120);
        return;
      }
      if (f == 1) {  // Delete
        text_t err = {0};
        bool ok = SdFileManager_deleteSelected(&err);
        if (!ok) {
          showNotification("Delete", (err).data);
          sys_sleep_ms(500);
        }
        SdFileManager_reloadDir(SdFileManager_cwd, NULL);
        SdFileManager_drawBrowserPage(true);
        sys_sleep_ms(120);
        return;
      }
    }
  }
}

void SdFileManager_setup(void) {
  SdFileManager_sdFmResetNavLabelCache();
  SdFileManager_page = SdFileManager_Page_Browser;
  SdFileManager_cwd = TEXT("/");
  SdFileManager_sel = 0;
  SdFileManager_uiDirty = true;
  SdFileManager_reloadDir(TEXT("/"), NULL);
  currentBatteryVoltage = readBatteryVoltage();
  drawStatusBar(currentBatteryVoltage, true, false);
  SdFileManager_drawBrowserPage(true);
}

void SdFileManager_loop(void) {
  updateStatusBar();
  if (featureHasTouchNavBar()) {
    maintainTouchNavBar();
  }
  SdFileManager_sdFmHandleNavButtons();
  if (feature_exit_requested) {
    return;
  }
  if (SdFileManager_uiDirty) {
    switch (SdFileManager_page) {
      case SdFileManager_Page_Browser:
        SdFileManager_drawBrowserPage(true);
        break;
      case SdFileManager_Page_Info:
        SdFileManager_drawInfoPage();
        break;
      case SdFileManager_Page_ConfirmDelete:
        SdFileManager_drawConfirmDeletePage();
        break;
    }
  }

  // Physical navigation (kept for devices without touch nav bar).
  if (!featureHasTouchNavBar() && isButtonPressed(BTN_UP)) {
    if (SdFileManager_page == SdFileManager_Page_Browser &&
        !((SdFileManager_entries).size == 0)) {
      SdFileManager_sel--;
      SdFileManager_clampSel();
      SdFileManager_drawBrowserPage(false);
    }
    sys_sleep_ms(160);
    return;
  }
  if (!featureHasTouchNavBar() && isButtonPressed(BTN_DOWN)) {
    if (SdFileManager_page == SdFileManager_Page_Browser &&
        !((SdFileManager_entries).size == 0)) {
      SdFileManager_sel++;
      SdFileManager_clampSel();
      SdFileManager_drawBrowserPage(false);
    }
    sys_sleep_ms(160);
    return;
  }

  int x, y;
  bool touched = readTouchXY(&(x), &(y));
  if (!touched) {
    if (SdFileManager_touchActive && !SdFileManager_touchDragging) {
      SdFileManager_handleTap(SdFileManager_touchStartX,
                              SdFileManager_touchStartY);
    }
    SdFileManager_touchActive = false;
    SdFileManager_touchDragging = false;
    SdFileManager_scrollAccumY = 0;
    return;
  }

  if (!SdFileManager_touchActive) {
    SdFileManager_touchActive = true;
    SdFileManager_touchDragging = false;
    SdFileManager_touchStartX = x;
    SdFileManager_touchStartY = y;
    SdFileManager_touchLastY = y;
    SdFileManager_scrollAccumY = 0;
    return;
  }

  if (SdFileManager_page == SdFileManager_Page_Browser) {
    int top, bottom, rowH, maxVisible;
    SdFileManager_listLayout(&(top), &(bottom), &(rowH), &(maxVisible));
    bool canScroll = (SdFileManager_touchStartY >= top &&
                      SdFileManager_touchStartY < bottom);

    int dy = y - SdFileManager_touchLastY;
    SdFileManager_touchLastY = y;

    if (!SdFileManager_touchDragging && canScroll) {
      if (abs(y - SdFileManager_touchStartY) > 8) {
        SdFileManager_touchDragging = true;
      }
    }

    if (SdFileManager_touchDragging && canScroll) {
      SdFileManager_scrollAccumY += dy;
      int steps = SdFileManager_scrollAccumY / rowH;
      if (steps != 0) {
        SdFileManager_listStart -= steps;
        SdFileManager_clampListStart(maxVisible);
        if (!((SdFileManager_entries).size == 0)) {
          if (SdFileManager_sel < SdFileManager_listStart)
            SdFileManager_sel = SdFileManager_listStart;
          if (SdFileManager_sel >= SdFileManager_listStart + maxVisible)
            SdFileManager_sel =
                DIV_MIN((int)(SdFileManager_entries).size - 1,
                        SdFileManager_listStart + maxVisible - 1);
        }
        SdFileManager_drawBrowserPage(false);
        SdFileManager_scrollAccumY -= steps * rowH;
      }
    }
  }

  return;
}
