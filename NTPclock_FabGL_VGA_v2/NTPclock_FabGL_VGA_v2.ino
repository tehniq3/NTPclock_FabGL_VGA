/*
 * inspirat de https://github.com/maksimKorzh/esp32-fullscreen-clock/
==========================================================================
  Ceas + Vreme pe ESP32 cu ieșire VGA (fabgl)
  ==========================================================================

  Funcționalitate:
  - Ceas digital mare cu cifre segment140 (HH:MM:SS în culori diferite)
  - Data în limba română (zi + dată completă) cu font dublu lățime
  - Date meteo în timp real de la open-meteo.com pentru Craiova
    * Temperatura (cu semn + și simbol °C)
    * Umiditatea relativă (%)
    * Presiunea atmosferică (mmHg, convertită din hPa)
    * Starea vremii în română (fără diacritice)
  - Antiflicker: data și vremea se redesenează doar când se schimbă

  Layout (canvas 512x247 linii, limitat de RAM-ul ESP32):
  - Y=  2-25 : Ziua săptămânii (alb, DoubleWidth)
  - Y= 28-51 : Data completă (alb)
  - Y= 54-77 : Vremea T+U+P (verde, DoubleWidth)
  - Y= 80-219 : Ceasul cu segment140 (roșu/alb/galben/alb/albastru)
  - Y= 221    : Linie separator gri
  - Y=223-246 : Starea vremii (gri deschis, DoubleWidth)

  Hardware:
  - Placă: ESP32 (orice variantă cu minim 4MB flash)
  - Modul VGA: DAC R2R pe GPIO 25, 26, 27 (sau modul GVM-2-VGA)
  - Conexiune: WiFi la rețeaua "bbk2" (modifică ssid/password mai jos)
  - Rezoluție: VGA 512x448@60Hz (canvas real ~247 linii din cauza memoriei)

  Biblioteci necesare:
  - fabgl  (de la fdivitto/FabGL pe GitHub)
  - WiFi, WiFiClient, HTTPClient  (incluse în ESP32 core)

  Configurare:
  - Modifică ssid/password în linia corespunzătoare
  - Pentru alt oraș: schimbă latitude/longitude în weatherURL
  - Fus orar România: EET-2EEST,M3.5.0/3,M10.5.0/4

  API meteo:
  - open-meteo.com (gratuit, fără cheie API)
  - Date reîmprospătate la fiecare 10 minute
  - Format: http://api.open-meteo.com/v1/forecast?latitude=...&longitude=...
    &current=temperature_2m,relative_humidity_2m,pressure_msl,weather_code

  Conversie presiune:
  - 1 hPa = 0.75006 mmHg  ⇒  presiune_mmHg = hPa × 0.75006

  Autor: Nicu FLORICA (niq_ro) + AI
  Data:  octombrie 2026
  Versiunea: 2.0
==========================================================================
*/

#include "fabgl.h"
#define FABGL_FONT_INCLUDE_DEFINITION
#include "segment140.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include "time.h"

const char* ssid       = "bbk2";
const char* password   = "internet2";
const char* ntpServer  = "pool.ntp.org";

const char* weatherURL =
  "http://api.open-meteo.com/v1/forecast?"
  "latitude=44.33&longitude=23.80"
  "&current=temperature_2m,relative_humidity_2m,pressure_msl,weather_code";

fabgl::VGAController DisplayController;
fabgl::Canvas        canvas(&DisplayController);

struct WeatherData {
  float  temperature;
  int    humidity;
  float  pressure;
  int    weatherCode;
  String description;
  bool   valid = false;
  bool   dirty = true;
};
static WeatherData currentWeather;
static const unsigned long WEATHER_FETCH_INTERVAL = 10UL * 60UL * 1000UL;

static char lastWeekday[20] = "";
static char lastDate[40]    = "";

void setTimezone(String timezone){
  Serial.printf("  Setting Timezone to %s\n",timezone.c_str());
  setenv("TZ",timezone.c_str(),1);
  tzset();
}

const char* getRomanianWeekday(int wday) {
  static const char* names[] = {
    "Duminica", "Luni", "Marti", "Miercuri",
    "Joi", "Vineri", "Sambata"
  };
  if (wday < 0 || wday > 6) return "?";
  return names[wday];
}

const char* getRomanianMonth(int mon) {
  static const char* names[] = {
    "Ianuarie",  "Februarie", "Martie",    "Aprilie",
    "Mai",       "Iunie",     "Iulie",     "August",
    "Septembrie","Octombrie", "Noiembrie", "Decembrie"
  };
  if (mon < 0 || mon > 11) return "?";
  return names[mon];
}

String weatherCodeToDescription(int code) {
  switch (code) {
    case 0:  return "Cer senin";
    case 1:  return "Aproape senin";
    case 2:  return "Partial noros";
    case 3:  return "Noros";
    case 45: return "Ceata";
    case 48: return "Ceata cu chiciura";
    case 51: return "Burnita usoara";
    case 53: return "Burnita moderata";
    case 55: return "Burnita densa";
    case 56: return "Burnita inghetata";
    case 57: return "Burnita inghetata";  // densa";
    case 61: return "Ploaie usoara";
    case 63: return "Ploaie moderata";
    case 65: return "Ploaie abundenta";
    case 66: return "Ploaie inghetata";  // usoara";
    case 67: return "Ploaie inghetata";  // densa";
    case 71: return "Ninsoare usoara";
    case 73: return "Ninsoare moderata";
    case 75: return "Ninsoare abundenta";
    case 77: return "Graunte de zapada";
    case 80: return "Averse usoare";
    case 81: return "Averse moderate";
    case 82: return "Averse violente";
    case 85: return "Averse de zapada";  // usoare";
    case 86: return "Averse de zapada:;  // abundente";
    case 95: return "Furtuna";
    case 96: return "Furtuna cu grindina";  // mica";
    case 99: return "Furtuna cu grindina";  // mare";
    default: return "Necunoscut";
  }
}

String extractValue(const String& json, const char* key) {
  int currentIdx = json.indexOf("\"current\":");
  if (currentIdx < 0) currentIdx = 0;
  String searchKey = String("\"") + key + "\":";
  int idx = json.indexOf(searchKey, currentIdx);
  if (idx < 0) return "";
  idx += searchKey.length();
  String value = "";
  while (idx < (int)json.length() &&
         json[idx] != ',' && json[idx] != '}') {
    value += json[idx];
    idx++;
  }
  value.trim();
  return value;
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Weather: WiFi indisponibil - sar peste fetch"));
    return;
  }
  Serial.printf("[Free heap: %u bytes] ", ESP.getFreeHeap());
  Serial.print(F("Fetching weather (Craiova)..."));

  WiFiClient client;
  client.setTimeout(8000);

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, weatherURL)) {
    Serial.println(F(" FAILED (begin)"));
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf(" FAILED (HTTP %d: %s)\n",
                  code, http.errorToString(code).c_str());
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.println(F(" OK"));
  Serial.println(payload);

  String tempStr = extractValue(payload, "temperature_2m");
  String humStr  = extractValue(payload, "relative_humidity_2m");
  String presStr = extractValue(payload, "pressure_msl");
  String codeStr = extractValue(payload, "weather_code");

  if (tempStr.length() == 0 || codeStr.length() == 0) {
    Serial.println(F("Weather: raspuns invalid - ignor"));
    return;
  }

  currentWeather.temperature  = tempStr.toFloat();
  currentWeather.humidity     = humStr.toInt();
  currentWeather.pressure     = presStr.toFloat();
  currentWeather.weatherCode  = codeStr.toInt();
  currentWeather.description  = weatherCodeToDescription(currentWeather.weatherCode);
  currentWeather.valid        = true;
  currentWeather.dirty         = true;
}

// ===== LAYOUT COMPRIMAT pentru canvas 247 linii =====
void drawDate(const char* weekday_buf, const char* date_buf)
{
  canvas.setBrushColor(RGB888(0, 0, 0));
  canvas.fillRectangle(0, 0, 511, 53);

  canvas.selectFont(&fabgl::FONT_std_24);
  canvas.setPenColor(Color::White);

  // Linia 1: ziua cu DoubleWidth (mai mare)
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.drawText(20, 2, weekday_buf);

  // Linia 2: data fără DoubleWidth (ca să încapă "20 Septembrie 2026")
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.drawText(20, 28, date_buf);
}

void drawClock(struct tm* timeinfo)
{
  char h_buf[3], m_buf[3], s_buf[3];
  sprintf(h_buf, "%02d", timeinfo->tm_hour);
  sprintf(m_buf, "%02d", timeinfo->tm_min);
  sprintf(s_buf, "%02d", timeinfo->tm_sec);

  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.selectFont(&fabgl::FONT_Seven_Segment140);

  canvas.setPenColor(Color::BrightRed);
  canvas.drawTextFmt(0,   80, "%s", h_buf);    // Y=54 (era 70)
  canvas.setPenColor(Color::White);
  canvas.drawTextFmt(140, 80, ":");
  canvas.setPenColor(Color::BrightYellow);
  canvas.drawTextFmt(190, 80, "%s", m_buf);
  canvas.setPenColor(Color::White);
  canvas.drawTextFmt(330, 80, ":");
  canvas.setPenColor(Color::BrightBlue);
  canvas.drawTextFmt(380, 80, "%s  ", s_buf);
}

void drawWeather()
{
  if (!currentWeather.valid || !currentWeather.dirty) return;

  // ===== PARTEA 1: T+U+P deasupra ceasului (verde, DoubleWidth) =====
  canvas.setBrushColor(RGB888(0, 0, 0));
  canvas.fillRectangle(0, 53, 511, 79);

  canvas.selectFont(&fabgl::FONT_std_24);
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );

  float pressureMmHg = currentWeather.pressure * 0.75006f;

  char line[80];
  snprintf(line, sizeof(line), "%+.1f%cC %d%% %.0fmmHg",
           currentWeather.temperature, 0xB0,
           currentWeather.humidity, pressureMmHg);

  canvas.setPenColor(Color::BrightGreen);
  canvas.drawText(10, 54, line);

  // ===== PARTEA 2: Starea vremii sub ceas (gri, DoubleWidth) =====
  canvas.setBrushColor(RGB888(0, 0, 0));
  canvas.fillRectangle(0, 220, 511, 246);

  // Linie subtilă de separare
  canvas.setBrushColor(RGB888(60, 60, 60));
  canvas.fillRectangle(0, 220, 511, 221);
  canvas.setBrushColor(RGB888(0, 0, 0));

  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)            // ← schimbat din false în true
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );

  canvas.setPenColor(RGB888(180, 180, 180));   // gri deschis
  canvas.drawText(10, 223, currentWeather.description.c_str());

  currentWeather.dirty = false;
}

void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

  char weekday_buf[20], date_buf[40];
  snprintf(weekday_buf, sizeof(weekday_buf), "%s",
           getRomanianWeekday(timeinfo.tm_wday));
  snprintf(date_buf, sizeof(date_buf), "%02d %s %d",
           timeinfo.tm_mday,
           getRomanianMonth(timeinfo.tm_mon),
           timeinfo.tm_year + 1900);

  if (strcmp(weekday_buf, lastWeekday) != 0 ||
      strcmp(date_buf,    lastDate)    != 0) {
    drawDate(weekday_buf, date_buf);
    strcpy(lastWeekday, weekday_buf);
    strcpy(lastDate,    date_buf);
  }

  drawClock(&timeinfo);
  drawWeather();
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

  delay(2000);

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

  Serial.printf("[Canvas] Width=%d Height=%d\n",
                canvas.getWidth(), canvas.getHeight());
  Serial.printf("[Free heap: %u bytes]\n", ESP.getFreeHeap());

  fetchWeather();
  printLocalTime();
}

void loop()
{
  static unsigned long lastWeatherFetch = 0;
  unsigned long now = millis();

  if (lastWeatherFetch == 0 ||
      (now - lastWeatherFetch) >= WEATHER_FETCH_INTERVAL) {
    fetchWeather();
    lastWeatherFetch = now;
  }

  printLocalTime();
  delay(1000);
}
