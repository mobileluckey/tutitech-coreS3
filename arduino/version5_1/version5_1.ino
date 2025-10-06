// TutiTech - CoreS3 (Sentences only)
// Uses prebuilt SQLite DB on SD at: /TTLesson1Thru3/TutiTech.db
// Flashcards removed. “Saved Lessons” button hidden (toggle SHOW_SAVED_BUTTON to 1 later).

#include <M5CoreS3.h>
#include <SD.h>
#include <SPI.h>
#include <sqlite3.h>
#include <esp_sleep.h>

// ---------- Touch constant (CoreS3) ----------
#ifndef TOUCH_BEGIN
#define TOUCH_BEGIN 3
#endif

// ---------- SD / DB paths ----------
#define SD_CS_PIN 4
static const char* DB_DIR  = "/TTLesson1Thru3";
static const char* DB_NAME = "TutiTech.db";   // exact file name on SD

// ---------- UI sizes & layout ----------
#define BUTTON_WIDTH   230
#define BUTTON_HEIGHT  44
#define BUTTON_GAP     16
#define MENU_TOP_Y     60

// Fonts (bitmap scaling factors)
#define FONT_TITLE_SIZE      3   // Splash + Menu title
#define FONT_DEFAULT_SIZE    2   // General UI
#define FONT_SENTENCE_SIZE   3   // Sentence text

// Back button (top-left)
const int BACK_W = 70;
const int BACK_H = 26;
const int BACK_X = 6;
const int BACK_Y = 6;

// Show/hide a placeholder Saved Lessons button
#define SHOW_SAVED_BUTTON 0   // set to 1 later if you implement Saved Lessons

// ---------- App state ----------
sqlite3* db = nullptr;
int rc = 0;

String currentSentence = "";
String currentTranslation = "";

enum ScreenState {
  SPLASH_SCREEN,
  MAIN_MENU,
  SENTENCE_PRACTICE,
  EXIT_CONFIRMATION
};
ScreenState currentScreen = SPLASH_SCREEN;

int prevTouchState = -1;

// ---------- Debug toggle ----------
#define DEBUG_LOGS 1
#if DEBUG_LOGS
  #define DBG(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif

// ---------- Helpers ----------
inline int SW() { return M5.Display.width(); }
inline int SH() { return M5.Display.height(); }

String sqlEsc(const String& s) { String r = s; r.replace("'", "''"); return r; }

void listDir(const char* path) {
  DBG("Listing %s\n", path);
  File d = SD.open(path);
  if (!d) { DBG("  (cannot open)\n"); return; }
  while (true) {
    File e = d.openNextFile();
    if (!e) break;
    DBG("  %s  (%u bytes)\n", e.name(), (unsigned)e.size());
    e.close();
  }
  d.close();
}

// Wrap and center a long string within a given width
void drawWrappedCentered(int yTop, const String& text, int maxWidth, int textSize, uint16_t color) {
  M5.Display.setTextSize(textSize);
  M5.Display.setTextColor(color);
  M5.Display.setTextDatum(top_left);

  String line = "";
  int xMargin = 10;
  int lineHeight = 12 * textSize + 4; // rough line height
  int cx = SW() / 2;

  int i = 0;
  while (i < (int)text.length()) {
    int j = i;
    while (j < (int)text.length() && text[j] != ' ') j++;
    String word = text.substring(i, j);
    String test = (line.length() == 0) ? word : (line + " " + word);

    int tw = M5.Display.textWidth(test.c_str());
    if (tw + 2 * xMargin <= maxWidth) {
      line = test;
    } else {
      int lw = M5.Display.textWidth(line.c_str());
      int x = cx - lw / 2;
      M5.Display.drawString(line, x, yTop);
      yTop += lineHeight;
      line = word;
    }

    if (j < (int)text.length() && text[j] == ' ') j++;
    i = j;
  }

  if (line.length() > 0) {
    int lw = M5.Display.textWidth(line.c_str());
    int x = cx - lw / 2;
    M5.Display.drawString(line, x, yTop);
  }
}

// UI: outlined button with centered label
void drawButtonOutline(const String& label, int x, int y, int w, int h, uint16_t border) {
  M5.Display.drawRect(x, y, w, h, border);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(FONT_DEFAULT_SIZE);
  M5.Display.setTextColor(WHITE);
  M5.Display.drawString(label, x + w/2, y + h/2);
  M5.Display.setTextDatum(top_left);
}

// Back button (top-left)
void drawBackButton() {
  M5.Display.drawRoundRect(BACK_X, BACK_Y, BACK_W, BACK_H, 6, YELLOW);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(FONT_DEFAULT_SIZE);
  M5.Display.setTextColor(WHITE);
  M5.Display.drawString("< Back", BACK_X + BACK_W/2, BACK_Y + BACK_H/2);
  M5.Display.setTextDatum(top_left);
}
bool inBackButton(int tx, int ty) {
  return (tx > BACK_X && tx < BACK_X + BACK_W && ty > BACK_Y && ty < BACK_Y + BACK_H);
}

// ---------- SQLite open/close ----------
bool openDatabaseInDir(const char* dirPath, const char* fileName) {
  if (db) { sqlite3_close(db); db = nullptr; }

  String sdPath = String(dirPath) + "/" + String(fileName);
  if (!SD.exists(sdPath.c_str())) {
    DBG("ERROR: %s missing on SD\n", sdPath.c_str());
    return false;
  }

  String sqlitePath = String("/sd") + sdPath;
  sqlite3_initialize();
  rc = sqlite3_open(sqlitePath.c_str(), &db);
  if (rc == SQLITE_OK && db) {
    DBG("DB open OK: %s\n", sqlitePath.c_str());
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;",     nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",  nullptr, nullptr, nullptr);

    // Ensure sentences.competed exists; ignore error if already present
    sqlite3_exec(db, "ALTER TABLE sentences ADD COLUMN completed INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

    // Boot sanity
    sqlite3_exec(db,
      "SELECT COUNT(*) FROM sentences;",
      [](void*,int argc,char** argv,char**)->int{
        DBG("[BOOT] sentences count = %s\n", argv[0]?argv[0]:"0");
        return 0;
      }, nullptr, nullptr);

    return true;
  }
  DBG("sqlite3_open failed (%s): rc=%d msg=%s\n",
      sqlitePath.c_str(), rc, db ? sqlite3_errmsg(db) : "(null)");
  if (db) { sqlite3_close(db); db = nullptr; }
  return false;
}

void closeDatabase() {
  if (db) { sqlite3_close(db); db = nullptr; DBG("DB closed.\n"); }
}

// ---------- Screens ----------
void setupSplashScreen() {
  currentScreen = SPLASH_SCREEN;
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextDatum(middle_center);

  // Title
  M5.Display.setTextColor(CYAN);
  M5.Display.setTextSize(FONT_TITLE_SIZE);
  M5.Display.drawString("TutiTech", SW()/2, SH()/2 - 16);

  // Subtitle
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(FONT_DEFAULT_SIZE);
  M5.Display.drawString("Language Tutor", SW()/2, SH()/2 + 12);

  M5.Display.setTextDatum(top_left);
}

void setupMainMenu() {
  currentScreen = MAIN_MENU;
  M5.Display.fillScreen(BLACK);

  // Title
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(FONT_TITLE_SIZE);
  M5.Display.drawString("Main Menu", SW()/2, 28);

  int x = (SW() - BUTTON_WIDTH)/2;
  int y = MENU_TOP_Y;

  // Start New Lesson
  drawButtonOutline("Start New Lesson", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, CYAN);
  y += BUTTON_HEIGHT + BUTTON_GAP;

#if SHOW_SAVED_BUTTON
  // Saved Lessons (placeholder)
  drawButtonOutline("Saved Lessons", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, CYAN);
  y += BUTTON_HEIGHT + BUTTON_GAP;
#endif

  // Exit
  drawButtonOutline("Exit", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, RED);

  M5.Display.setTextDatum(top_left);
}

static int sentenceCB(void* notUsed, int argc, char** argv, char** col) {
  (void)notUsed; (void)col;
  if (argc >= 2) {
    currentSentence    = argv[0] ? argv[0] : "";
    currentTranslation = argv[1] ? argv[1] : "";
  }
  return 0;
}

void loadNextSentence() {
  currentSentence = "";
  currentTranslation = "";

  const char* q =
    "SELECT sentence, translation FROM sentences "
    "WHERE COALESCE(completed,0) = 0 "
    "ORDER BY lesson_number ASC, sequence ASC LIMIT 1;";
  char* err = nullptr;
  int r = sqlite3_exec(db, q, sentenceCB, nullptr, &err);
  if (r != SQLITE_OK) {
    DBG("Sentence query err: %s\n", err ? err : "(null)");
    if (err) sqlite3_free(err);
  }
}

void sentencePracticeScreen() {
  currentScreen = SENTENCE_PRACTICE;
  M5.Display.fillScreen(BLACK);

  // Back button
  drawBackButton();

  // Load next (or show completed)
  loadNextSentence();

  M5.Display.setTextDatum(middle_center);

  // Heading
  M5.Display.setTextColor(CYAN);
  M5.Display.setTextSize(FONT_DEFAULT_SIZE);
  M5.Display.drawString("Repeat after me:", SW()/2, 36);

  if (currentSentence.length() == 0) {
    // All complete
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(FONT_DEFAULT_SIZE);
    M5.Display.drawString("All sentences completed!", SW()/2, 80);

    // Offer reset progress
    int x = (SW() - BUTTON_WIDTH)/2;
    drawButtonOutline("Reset Lessons", x, 130, BUTTON_WIDTH, BUTTON_HEIGHT, YELLOW);
  } else {
    // Sentence (wrapped, centered)
    drawWrappedCentered(70, currentSentence, SW() - 20, FONT_SENTENCE_SIZE, WHITE);
    // Translation smaller
    drawWrappedCentered(120, currentTranslation, SW() - 30, FONT_DEFAULT_SIZE, 0x7BEF);

    // Next button
    int x = (SW() - BUTTON_WIDTH)/2;
    drawButtonOutline("Next", x, SH() - 52, BUTTON_WIDTH, BUTTON_HEIGHT, GREEN);
  }

  M5.Display.setTextDatum(top_left);
}

void setupFarewellScreen() {
  currentScreen = EXIT_CONFIRMATION;
  M5.Display.fillScreen(BLACK);

  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(FONT_TITLE_SIZE);
  M5.Display.drawString("Exit TutiTech?", SW()/2, 70);

  int x = (SW() - BUTTON_WIDTH)/2;
  drawButtonOutline("Yes", x, 120, BUTTON_WIDTH, BUTTON_HEIGHT, YELLOW);
  drawButtonOutline("No",  x, 120 + BUTTON_HEIGHT + BUTTON_GAP, BUTTON_WIDTH, BUTTON_HEIGHT, YELLOW);

  M5.Display.setTextDatum(top_left);
}

// ---------- Touch handling ----------
void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (prevTouchState != t.state) {
    prevTouchState = t.state;

    if (t.state == TOUCH_BEGIN) {
      M5.Speaker.tone(750, 80);
      int tx = t.x, ty = t.y;

      // Back works in all screens except main menu
      if (currentScreen == SENTENCE_PRACTICE || currentScreen == EXIT_CONFIRMATION) {
        if (inBackButton(tx, ty)) {
          setupMainMenu();
          return;
        }
      }

      if (currentScreen == MAIN_MENU) {
        int x = (SW() - BUTTON_WIDTH)/2;
        int y1 = MENU_TOP_Y;
        int y = y1;

        // Start New Lesson
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          sentencePracticeScreen();
          return;
        }
        y += BUTTON_HEIGHT + BUTTON_GAP;

#if SHOW_SAVED_BUTTON
        // Saved Lessons (placeholder)
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          // Placeholder screen
          M5.Display.fillScreen(BLACK);
          drawBackButton();
          M5.Display.setTextDatum(middle_center);
          M5.Display.setTextColor(WHITE);
          M5.Display.setTextSize(FONT_DEFAULT_SIZE);
          M5.Display.drawString("No saved lessons yet.", SW()/2, SH()/2);
          M5.Display.setTextDatum(top_left);
          return;
        }
        y += BUTTON_HEIGHT + BUTTON_GAP;
#endif

        // Exit
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          setupFarewellScreen();
          return;
        }
      }
      else if (currentScreen == SENTENCE_PRACTICE) {
        if (currentSentence.length() == 0) {
          // On the "All completed" view: Reset Lessons?
          int x = (SW() - BUTTON_WIDTH)/2;
          int y = 130;
          if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
            char* err = nullptr;
            sqlite3_exec(db, "UPDATE sentences SET completed=0;", nullptr, nullptr, &err);
            if (err) { DBG("Reset err: %s\n", err); sqlite3_free(err); }
            sentencePracticeScreen(); // reload
          }
        } else {
          // Next
          int nx = (SW() - BUTTON_WIDTH)/2;
          int ny = SH() - 52;
          if (tx > nx && tx < nx + BUTTON_WIDTH && ty > ny && ty < ny + BUTTON_HEIGHT) {
            String q = "UPDATE sentences SET completed=1 WHERE sentence='" +
                       sqlEsc(currentSentence) + "' AND translation='" + sqlEsc(currentTranslation) + "';";
            char* err = nullptr;
            int r = sqlite3_exec(db, q.c_str(), nullptr, nullptr, &err);
            if (r != SQLITE_OK && err) { DBG("Update err: %s\n", err); sqlite3_free(err); }
            sentencePracticeScreen();
          }
        }
      }
      else if (currentScreen == EXIT_CONFIRMATION) {
        int x = (SW() - BUTTON_WIDTH)/2;
        int yYes = 120;
        int yNo  = 120 + BUTTON_HEIGHT + BUTTON_GAP;

        if (tx > x && tx < x + BUTTON_WIDTH && ty > yYes && ty < yYes + BUTTON_HEIGHT) {
          M5.Display.fillScreen(BLACK);
          M5.Display.setTextDatum(middle_center);
          M5.Display.setTextColor(WHITE);
          M5.Display.setTextSize(FONT_DEFAULT_SIZE);
          M5.Display.drawString("Goodbye!", SW()/2, SH()/2);
          M5.Display.setTextDatum(top_left);
          delay(800);
          closeDatabase();
          esp_sleep_enable_timer_wakeup(5ULL * 1000ULL * 1000ULL);
          esp_light_sleep_start();
          setupMainMenu();
        }
        else if (tx > x && tx < x + BUTTON_WIDTH && ty > yNo && ty < yNo + BUTTON_HEIGHT) {
          setupMainMenu();
        }
      }
    }
  }
}

// ---------- setup / loop ----------
void setup() {
  M5.begin();
  Serial.begin(115200);

  // Lock a known landscape rotation so centering math is stable
  M5.Display.setRotation(1);

  Serial.println(SD.begin(SD_CS_PIN) ? "SD.begin OK" : "SD.begin FAIL");
  listDir("/");
  listDir(DB_DIR);

  String dbPath = String(DB_DIR) + "/" + String(DB_NAME);
  if (!SD.exists(dbPath.c_str())) {
    Serial.println("ERROR: /TTLesson1Thru3/TutiTech.db NOT FOUND on SD.");
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(RED);
    M5.Display.setTextSize(FONT_DEFAULT_SIZE);
    M5.Display.setCursor(10, 100);
    M5.Display.println("Missing DB on SD:");
    M5.Display.println("/TTLesson1Thru3/TutiTech.db");
    while (true) { delay(1000); }
  }

  if (!openDatabaseInDir(DB_DIR, DB_NAME)) {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(RED);
    M5.Display.setTextSize(FONT_DEFAULT_SIZE);
    M5.Display.setCursor(10, 110);
    M5.Display.println("DB open failed:");
    M5.Display.println("/TTLesson1Thru3/TutiTech.db");
    while (true) { delay(1000); }
  }

  setupSplashScreen();
  delay(900);
  setupMainMenu();
}

void loop() {
  M5.update();
  handleTouch();
}

