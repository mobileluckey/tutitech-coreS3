// TutiTech - CoreS3 (Sentences + Flashcards)  [with Continue + Auto Light Sleep]
// Uses prebuilt SQLite DB on SD at: /TTLesson1Thru3/TutiTech.db
// Saved Lessons button still hidden (toggle SHOW_SAVED_BUTTON to 1 later).

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
#define BUTTON_HEIGHT  40
#define BUTTON_GAP     10
#define MENU_TOP_Y     48

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

// >>> NEW: Flashcard state
long   currentFlashRowId = -1;
String currentTerm = "";
String optA = "";
String optB = "";
int    correctIndex = 1; // 1 or 2 (refers to defs in DB before shuffle)
bool   shuffledSwap = false; // if true, A/B are swapped on screen

enum ScreenState {
  SPLASH_SCREEN,
  MAIN_MENU,
  SENTENCE_PRACTICE,
  FLASHCARD_PRACTICE,   
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

// ---------- Power / inactivity ----------
static const uint32_t INACTIVITY_MS   = 120000; // 2 minutes without touch -> sleep
static const uint32_t AUTO_SLEEP_SECS = 30;     // sleep duration in seconds
uint32_t lastInteractionMs = 0;
uint8_t defaultBrightness = 200; // remember & restore

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

    // Ensure sentences.completed exists; ignore error if already present
    sqlite3_exec(db, "ALTER TABLE sentences ADD COLUMN completed INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

    // >>> NEW: Ensure flashcards.completed exists
    sqlite3_exec(db, "ALTER TABLE flashcards ADD COLUMN completed INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);

    // Boot sanity
    sqlite3_exec(db,
      "SELECT COUNT(*) FROM sentences;",
      [](void*,int argc,char** argv,char**)->int{
        DBG("[BOOT] sentences count = %s\n", argv[0]?argv[0]:"0");
        return 0;
      }, nullptr, nullptr);

    sqlite3_exec(db,
      "SELECT COUNT(*) FROM flashcards;",
      [](void*,int argc,char** argv,char**)->int{
        DBG("[BOOT] flashcards count = %s\n", argv[0]?argv[0]:"0");
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

  // CONTINUE LESSON
  drawButtonOutline("Continue Lesson", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, CYAN);
  y += BUTTON_HEIGHT + BUTTON_GAP;

  // >>> NEW: FLASHCARDS
  drawButtonOutline("Flashcards", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, CYAN);
  y += BUTTON_HEIGHT + BUTTON_GAP;

  // Start New Lesson
  drawButtonOutline("Start New Lesson", x, y, BUTTON_WIDTH, BUTTON_HEIGHT, CYAN);
  y += BUTTON_HEIGHT + BUTTON_GAP;

#if SHOW_SAVED_BUTTON
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

// >>> NEW: Flashcards loader
static int flashcardCB(void* notUsed, int argc, char** argv, char** col) {
  (void)notUsed; (void)col;
  // argv: [0]=rowid, [1]=term, [2]=d1, [3]=d2, [4]=ci
  currentFlashRowId = -1;
  currentTerm = optA = optB = "";
  correctIndex = 1;
  shuffledSwap = false;

  if (argc >= 5) {
    currentFlashRowId = argv[0] ? atol(argv[0]) : -1;
    currentTerm       = argv[1] ? argv[1] : "";
    String d1         = argv[2] ? argv[2] : "";
    String d2         = argv[3] ? argv[3] : "";
    correctIndex      = argv[4] ? atoi(argv[4]) : 1; // expects 1 or 2

    // Randomly swap A/B on screen to avoid position bias
    if (esp_random() & 1) {
      optA = d2; optB = d1; shuffledSwap = true;
    } else {
      optA = d1; optB = d2; shuffledSwap = false;
    }
  }
  return 0;
}

void loadNextFlashcard() {
  currentFlashRowId = -1;
  currentTerm = optA = optB = "";
  correctIndex = 1;
  shuffledSwap = false;

  const char* q =
    "SELECT rowid, term, "
    "COALESCE(def1, definition_1), "
    "COALESCE(def2, definition_2), "
    "COALESCE(correct_index, correct_def) "
    "FROM flashcards "
    "WHERE COALESCE(completed,0)=0 "
    "ORDER BY rowid ASC LIMIT 1;";
  char* err = nullptr;
  int r = sqlite3_exec(db, q, flashcardCB, nullptr, &err);
  if (r != SQLITE_OK) {
    DBG("Flashcard query err: %s\n", err ? err : "(null)");
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

// >>> NEW: Flashcard practice screen
void flashcardPracticeScreen() {
  currentScreen = FLASHCARD_PRACTICE;
  M5.Display.fillScreen(BLACK);

  drawBackButton();

  loadNextFlashcard();

  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CYAN);
  M5.Display.setTextSize(FONT_DEFAULT_SIZE);
  M5.Display.drawString("Flashcard", SW()/2, 36);

  if (currentFlashRowId < 0 || currentTerm.length() == 0) {
    // No more cards
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(FONT_DEFAULT_SIZE);
    M5.Display.drawString("All flashcards completed!", SW()/2, 86);

    int x = (SW() - BUTTON_WIDTH)/2;
    drawButtonOutline("Reset Flashcards", x, 130, BUTTON_WIDTH, BUTTON_HEIGHT, YELLOW);
    M5.Display.setTextDatum(top_left);
    return;
  }

  // Term big & centered
  drawWrappedCentered(70, currentTerm, SW() - 20, FONT_SENTENCE_SIZE, WHITE);

  // Two answer buttons A/B
  int x = (SW() - BUTTON_WIDTH)/2;
  int yA = SH() - 120;
  int yB = SH() - 60;

  drawButtonOutline(optA, x, yA, BUTTON_WIDTH, BUTTON_HEIGHT, GREEN);
  drawButtonOutline(optB, x, yB, BUTTON_WIDTH, BUTTON_HEIGHT, GREEN);

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

// ---------- Light sleep helpers ----------
void goToLightSleepSeconds(uint32_t secs) {
  uint8_t prev = defaultBrightness;
  M5.Display.setBrightness(10);
  DBG("Entering light sleep for %lu seconds...\n", (unsigned long)secs);

  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000ULL * 1000ULL);
  esp_light_sleep_start();  // returns after wake

  DBG("Woke from light sleep.\n");
  M5.Display.setBrightness(prev);
  setupMainMenu();
}

void markInteraction() { lastInteractionMs = millis(); }

// ---------- Touch handling ----------
void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (prevTouchState != t.state) {
    prevTouchState = t.state;

    if (t.state == TOUCH_BEGIN) {
      markInteraction();
      M5.Speaker.tone(750, 80);
      int tx = t.x, ty = t.y;

      // Back works in these screens
      if (currentScreen == SENTENCE_PRACTICE || currentScreen == FLASHCARD_PRACTICE || currentScreen == EXIT_CONFIRMATION) {
        if (inBackButton(tx, ty)) {
          setupMainMenu();
          return;
        }
      }

      if (currentScreen == MAIN_MENU) {
        int x = (SW() - BUTTON_WIDTH)/2;
        int y = MENU_TOP_Y;

        // CONTINUE LESSON
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          sentencePracticeScreen();
          return;
        }
        y += BUTTON_HEIGHT + BUTTON_GAP;

        // FLASHCARDS  >>> NEW
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          flashcardPracticeScreen();
          return;
        }
        y += BUTTON_HEIGHT + BUTTON_GAP;

        // START NEW LESSON
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
          char* err = nullptr;
          int r = sqlite3_exec(db, "UPDATE sentences SET completed=0;", nullptr, nullptr, &err);
          if (r != SQLITE_OK && err) { DBG("Reset (Start New) err: %s\n", err); sqlite3_free(err); }
          sentencePracticeScreen();
          return;
        }
        y += BUTTON_HEIGHT + BUTTON_GAP;

#if SHOW_SAVED_BUTTON
        if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
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
          // Reset Lessons
          int x = (SW() - BUTTON_WIDTH)/2;
          int y = 130;
          if (tx > x && tx < x + BUTTON_WIDTH && ty > y && ty < y + BUTTON_HEIGHT) {
            char* err = nullptr;
            sqlite3_exec(db, "UPDATE sentences SET completed=0;", nullptr, nullptr, &err);
            if (err) { DBG("Reset err: %s\n", err); sqlite3_free(err); }
            sentencePracticeScreen();
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
      else if (currentScreen == FLASHCARD_PRACTICE) {
        int x = (SW() - BUTTON_WIDTH)/2;
        int yA = SH() - 120;
        int yB = SH() - 60;

        if (currentFlashRowId < 0 || currentTerm.length() == 0) {
          // Reset Flashcards button
          int ry = 130;
          if (tx > x && tx < x + BUTTON_WIDTH && ty > ry && ty < ry + BUTTON_HEIGHT) {
            char* err = nullptr;
            sqlite3_exec(db, "UPDATE flashcards SET completed=0;", nullptr, nullptr, &err);
            if (err) { DBG("Flash reset err: %s\n", err); sqlite3_free(err); }
            flashcardPracticeScreen();
          }
          return;
        }

        // Determine which button pressed (A or B)
        bool pressedA = (tx > x && tx < x + BUTTON_WIDTH && ty > yA && ty < yA + BUTTON_HEIGHT);
        bool pressedB = (tx > x && tx < x + BUTTON_WIDTH && ty > yB && ty < yB + BUTTON_HEIGHT);
        if (pressedA || pressedB) {
          bool isCorrectShownOnA;
          // If DB says correctIndex=1 means original d1 is correct.
          // If we swapped, then A shows d2; so A is correct iff (correctIndex==2 && swapped), else if (!swapped && correctIndex==1)
          if (!shuffledSwap) {
            isCorrectShownOnA = (correctIndex == 1);
          } else {
            isCorrectShownOnA = (correctIndex == 2);
          }
          bool correct = (pressedA && isCorrectShownOnA) || (pressedB && !isCorrectShownOnA);

          // Quick feedback flash (border color)
          uint16_t color = correct ? GREEN : RED;
          M5.Display.drawRect(0, 0, SW(), SH(), color);
          if (correct) {
            // mark completed by rowid
            String q = "UPDATE flashcards SET completed=1 WHERE rowid=" + String(currentFlashRowId) + ";";
            char* err = nullptr;
            int r = sqlite3_exec(db, q.c_str(), nullptr, nullptr, &err);
            if (r != SQLITE_OK && err) { DBG("Flash complete err: %s\n", err); sqlite3_free(err); }
          }
          delay(140);
          flashcardPracticeScreen(); // load next
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

          openDatabaseInDir(DB_DIR, DB_NAME);
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
  defaultBrightness = 200;
  M5.Display.setBrightness(defaultBrightness);

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

  markInteraction();
}

void loop() {
  M5.update();
  handleTouch();

  // Auto light-sleep on inactivity
  if (millis() - lastInteractionMs > INACTIVITY_MS) {
    goToLightSleepSeconds(AUTO_SLEEP_SECS);
    markInteraction();
  }
}
