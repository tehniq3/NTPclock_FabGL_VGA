/*
 * inspirat de https://github.com/maksimKorzh/esp32-fullscreen-clock/
==========================================================================
  Ceas pe ESP32 cu ieșire VGA (fabgl)
  ==========================================================================
  Autor: Nicu FLORICA (niq_ro) + AI
  Data:  octombrie 2026
  Versiunea: 2.0
==========================================================================
*/


#include "fabgl.h"
#define FABGL_FONT_INCLUDE_DEFINITION
#include "segment140.h"
#include <WiFi.h>
#include "time.h"

const char* ssid       = "bbk2";
const char* password   = "internet2";
const char* ntpServer  = "pool.ntp.org";
fabgl::VGAController DisplayController;
fabgl::Canvas        canvas(&DisplayController);

// Variabile statice pentru detectarea schimbării datei (anti-flicker)
static char lastWeekday[20] = "";
static char lastDate[40]    = "";

void setTimezone(String timezone){
  Serial.printf("  Setting Timezone to %s\n",timezone.c_str());
  setenv("TZ",timezone.c_str(),1);
  tzset();
}

void drawDate(const char* weekday_buf, const char* date_buf)
{
  // Curățăm fundalul extins pentru cele 2 rânduri de dată
  canvas.setBrushColor(RGB888(0, 0, 0));
  canvas.fillRectangle(0, 25, 512, 95);

  // Selectăm fontul 24px (cel mai mare nativ din FabGL)
  canvas.selectFont(&fabgl::FONT_std_24);
  canvas.setPenColor(Color::White);

  // DoubleWidth dublează lățimea caracterelor
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );

  // Linia 1: ziua săptămânii
  canvas.drawText(20, 28, weekday_buf);
  // Linia 2: data completă (inclusiv anul)
  canvas.drawText(20, 62, date_buf);
}

void drawClock(struct tm* timeinfo)
{
  char h_buf[3];
  char m_buf[3];
  char s_buf[3];

  sprintf(h_buf, "%02d", timeinfo->tm_hour);
  sprintf(m_buf, "%02d", timeinfo->tm_min);
  sprintf(s_buf, "%02d", timeinfo->tm_sec);

  // Resetăm opțiunile glyph pentru ceas (fără DoubleWidth)
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.selectFont(&fabgl::FONT_Seven_Segment140);

  // Curățăm zona ceasului (Anti-Flicker pentru ceas)
  canvas.setBrushColor(RGB888(0, 0, 0));
 // canvas.fillRectangle(0, 125, 512, 280);

  canvas.setPenColor(Color::BrightRed);
  canvas.drawTextFmt(0, 130, "%s", h_buf);

  canvas.setPenColor(Color::White);
  canvas.drawTextFmt(140, 130, ":");

  canvas.setPenColor(Color::BrightYellow);
  canvas.drawTextFmt(190, 130, "%s", m_buf);

  canvas.setPenColor(Color::White);
  canvas.drawTextFmt(330, 130, ":");

  canvas.setPenColor(Color::BrightBlue);
  canvas.drawTextFmt(380, 130, "%s  ", s_buf);
}

void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

  // 1. GENERARE DATĂ
  char weekday_buf[20];
  char date_buf[40];

  strftime(weekday_buf, sizeof(weekday_buf), "%A", &timeinfo);
  strftime(date_buf,    sizeof(date_buf),    "%d %B %Y", &timeinfo);

  // ===== ANTI-FLICKER: redesenăm data DOAR când se schimbă =====
  if (strcmp(weekday_buf, lastWeekday) != 0 ||
      strcmp(date_buf,    lastDate)    != 0) {
    drawDate(weekday_buf, date_buf);
    strcpy(lastWeekday, weekday_buf);
    strcpy(lastDate,    date_buf);
  }

  // 2. DESENEAZĂ CEASUL (în fiecare secundă)
  drawClock(&timeinfo);
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.printf("Connecting to %s ", ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }
  Serial.println(" CONNECTED");

  configTime(0, 0, ntpServer);
  setTimezone("EET-2EEST,M3.5.0/3,M10.5.0/4");

  Serial.print("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[OK] Time successfully synchronized!");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);

  DisplayController.begin();
  DisplayController.setResolution(VGA_512x448_60Hz);
  canvas.setBrushColor(RGB888(0, 0, 0));
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );

  canvas.clear();

  printLocalTime();
}

void loop()
{
  delay(1000);
  printLocalTime();
}
