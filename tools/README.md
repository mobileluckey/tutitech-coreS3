# TutiTech — CoreS3 (Sentences-only demo)

**What it is:**  
An ESP32-S3 (M5Stack CoreS3) sketch that loads an SQLite database from SD and runs a simple “Start New Lesson” sentences flow. Flashcards currently disabled.

**Hardware**
- M5Stack CoreS3
- microSD card
- SD contents: `/TTLesson1Thru3` with `TutiTech.db` (built on PC) and CSVs if needed

**Build (Arduino IDE)**
1. Boards Manager: ESP32 by Espressif (tested with 3.3.1)
2. Libraries (Library Manager):
   - `M5CoreS3`
   - `SQLite3` (ESP32 port by siara-cc)
3. Open `arduino/version5_1/version5_1.ino`
4. Flash to device
5. SD card should contain `/TTLesson1Thru3/TutiTech.db`

**SD layout**
