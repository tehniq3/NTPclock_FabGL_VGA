/*
 * inspirat de https://github.com/maksimKorzh/esp32-fullscreen-clock/
 * ==========================================================================
 *  Ceas + Vreme pe ESP32 cu ieșire VGA (fabgl) - VERSIUNE 16 CULORI
 * ==========================================================================
 *
 *  MODIFICĂRI vs v.2.0:
 *  - folosește VGA16Controller (4bpp = 16 culori) în loc de VGAController (8bpp)
 *  - rezoluție VGA 640x388 (fabgl alocă 388 linii la 640 lățime din cauza RAM)
 *  - RAM necesar: ~123KB (vs ~300KB) → încap TOATE informațiile pe ecran
 *  - folosește exclusiv Color:: enum (paletă 16 culori VGA)
 *  - adăugate informații meteo complete: vânt, UV, calitate aer, PM2.5/PM10
 *  - layout reorganizat pe toată suprafața 640x388
 *  - ANTIFLICKER: doar cifra schimbată se redesenează (nu toată zona ceasului)
 *  - vânt, UV, AQI, PM pe rânduri separate
 *
 *  v.3.1 - bugfix: layout adaptat la Height=388 (nu 480), antiflicker pe cifră
 *
 *  Funcționalitate:
 *  - Ceas digital mare cu segment140 (HH:MM:SS în 4 culori diferite)
 *  - Data în limba română (zi + dată completă)
 *  - Date meteo de la open-meteo.com pentru Craiova:
 *    * Temperatura + umiditatea + presiunea (mmHg)
 *    * Viteza + direcția vântului
 *    * Indice UV cu descriere
 *    * Calitatea aerului (European AQI) cu descriere
 *    * PM2.5 și PM10
 *    * Starea vremii în română
 *  - Antiflicker: redesenează doar ce s-a schimbat
 *
 *  Layout (canvas 640x388 - fabgl alocă 388 linii la 640 lățime):
 *  - Y=  0-26  : Ziua săptămânii + data (alb + galben)
 *  - Y= 28-54  : T+U+P (verde, DoubleWidth)
 *  - Y= 58-198 : HH:MM segment140 + DoubleWidth (140px înălțime, roșu+galben)
 *                2 puncte ":" (pulsează) între HH și MM
 *  - Y=200-340 : SS segment140 normal (140px, albastru, sub M2)
 *  - Y=220-246 : Vânt (cyan)
 *  - Y=248-274 : Indice UV (galben)
 *  - Y=276-302 : Calitate aer AQI (magenta)
 *  - Y=304-330 : PM2.5 (roșu, fără spațiu val/unit)
 *  - Y=332-358 : PM10 (roșu, sub PM2.5, fără spațiu val/unit)
 *  - Y=360-386 : Starea vremii (alb)
 *
 *  Paletă 16 culori (Color:: enum din fabgl):
 *  Black, White, Red, Green, Blue, Yellow, Cyan, Magenta,
 *  BrightRed, BrightGreen, BrightBlue, BrightYellow, BrightCyan, BrightMagenta,
 *  DarkGray, LightGray
 *
 *  Hardware:
 *  - ESP32 (minim 4MB flash, recomandat 4MB+ PSRAM)
 *  - Modul VGA: DAC R2R pe pinii default fabgl VGA16Controller:
 *      R: GPIO22, GPIO21
 *      G: GPIO19, GPIO18
 *      B: GPIO5,  GPIO4
 *      HSync: GPIO23, VSync: GPIO15
 *    (compatibil TTGO VGA32)
 *
 *  Biblioteci necesare:
 *  - fabgl  (de la fdivitto/FabGL pe GitHub)
 *  - WiFi, WiFiClient, HTTPClient  (incluse în ESP32 core)
 *  - segment140.h (font personalizat)
 *
 *  Configurare:
 *  - Modifică ssid/password mai jos
 *  - Pentru alt oraș: schimbă latitude/longitude în weatherURL și aqiURL
 *  - Fus orar România: EET-2EEST,M3.5.0/3,M10.5.0/4
 *
 *  API meteo (gratuit, fără cheie):
 *  - open-meteo.com (vremea)
 *  - air-quality-api.open-meteo.com (calitatea aerului)
 *  - Reîmprospătare la fiecare 10 minute
 *
 *  Autor: Nicu FLORICA (niq_ro) + AI
 *  Data:  octombrie 2026
 *  Versiunea: 3.0 (16 culori, full VGA 640x480)
 * ==========================================================================
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

// Coordonate Craiova
const char* weatherURL =
  "http://api.open-meteo.com/v1/forecast?"
  "latitude=44.317&longitude=23.80"
  "&current=temperature_2m,relative_humidity_2m,surface_pressure,pressure_msl,"
  "weather_code,wind_speed_10m,wind_direction_10m,uv_index,is_day&timezone=auto";

const char* aqiURL =
  "http://air-quality-api.open-meteo.com/v1/air-quality?"
  "latitude=44.317&longitude=23.80"
  "&current=european_aqi,pm2_5,pm10&timezone=auto";

// ===== CONTROLLER PE 16 CULORI (4bpp) =====
// În loc de VGAController (8bpp/256 culori), folosim VGA16Controller (4bpp/16 culori)
// Asta reduce RAM-ul de la ~300KB la ~153KB pentru 640x480,
// permițând rezoluție full VGA pe ESP32 standard.
fabgl::VGA16Controller DisplayController;
fabgl::Canvas          canvas(&DisplayController);

struct WeatherData {
  float  temperature;
  int    humidity;
  float  pressure;
  int    weatherCode;
  String description;
  float  windSpeed;
  int    windDir;
  String windDirDesc;
  float  uvIndex;
  String uvDesc;
  int    isDay;
  int    euAqi;
  String aqiDesc;
  float  pm25;
  float  pm10;
  bool   valid = false;
  bool   dirty = true;
};

static WeatherData currentWeather;
static const unsigned long WEATHER_FETCH_INTERVAL = 10UL * 60UL * 1000UL;

static char lastWeekday[20] = "";
static char lastDate[40]    = "";

// Antiflicker ceas: ținem minte valorile anterioare ale fiecărei cifre
// ca să redesenăm DOAR cifra care s-a schimbat (nu toată zona ceasului)
static int  prev_h1 = -1, prev_h2 = -1;
static int  prev_m1 = -1, prev_m2 = -1;
static int  prev_s  = -1;  // pt secunde (si bataia colonului)
static bool clock_first_draw = true;

// Poziții cifre ceas
// v.4.0: HH și MM amândouă cu segment140 + DoubleWidth (140px înălțime).
//        Renunț la primitive grafice (nu mai e nevoie de înălțime dublă).
//        SS sub M2 cu segment140 normal (140px, fără DW).
//        Bataia = 2 puncte primitive între HH și MM.
const int CLK_Y         = 58;     // top HH:MM (segment140)
const int CLK_H         = 140;    // înălțime HH:MM (segment140 + DW)

// HH:MM cu segment140 + DoubleWidth (ca în v.3.5)
const int H1_X          = 25;     // zeci ore (DoubleWidth)
const int H2_X          = 165;    // unități ore (gap 140)
const int M1_X          = 335;    // zeci minute (DoubleWidth)
const int M2_X          = 475;    // unități minute (gap 140)
const int DIGIT_W_DW    = 140;    // lățime estimată cifră cu DoubleWidth

// Separator ":" = 2 puncte primitive care pulsează (ca în codul P10 original)
const int COL_DOT_X     = 300;    // X puncte (între HH și MM)
const int COL_DOT_Y1    = 95;     // Y punct sus (mijloc sus)
const int COL_DOT_Y2    = 145;    // Y punct jos
const int COL_DOT_W     = 12;     // lățime punct
const int COL_DOT_H     = 12;     // înălțime punct

// SS sub M2 (unitățile de minute) cu segment140 normal, fără DoubleWidth
const int SS_Y          = 200;    // sub MM (gap 2px)
const int SS_H          = 140;    // înălțime segment140 normal
const int S1_X          = 475;    // zeci secunde (aliniat sub M2)
const int S2_X          = 535;    // unități secunde (gap 60, fără DW)
const int DIGIT_W_NORM  = 50;     // lățime estimată cifră segment140 normală

// Hartă segmente per cifră (7-segment)
// Index: 0=a (sus), 1=b (dr-sus), 2=c (dr-jos), 3=d (jos),
//        4=e (st-jos), 5=f (st-sus), 6=g (mijloc)
static const bool segMap[10][7] = {
  {1,1,1,1,1,1,0}, // 0
  {0,1,1,0,0,0,0}, // 1
  {1,1,0,1,1,0,1}, // 2
  {1,1,1,1,0,0,1}, // 3
  {0,1,1,0,0,1,1}, // 4
  {1,0,1,1,0,1,1}, // 5
  {1,0,1,1,1,1,1}, // 6
  {1,1,1,0,0,0,0}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}, // 9
};

// Desenează o cifră 7-segment cu primitive grafice (control total dimensiuni)
// (x, y) = colțul stânga-sus al cifrei, w = lățime, h = înălțime, t = grosime segment
void draw7Segment(int x, int y, int digit, int w, int h, int t, Color color) {
  if (digit < 0 || digit > 9) return;

  canvas.setBrushColor(color);

  int halfH    = h / 2;
  int xSegL    = x + t;          // capăt stânga segment orizontal
  int xSegR    = x + w - t;      // capăt dreapta segment orizontal
  int yTopHalf = y + t;          // top jumătate sus (sub segmentul a)
  int yMidTop  = y + halfH - t/2; // top segment g (sus)
  int yMidBot  = y + halfH + t/2; // bottom segment g (jos)
  int yBotHalf = y + h - t;       // top jumătate jos (deasupra segmentului d)

  // a: segment orizontal sus
  if (segMap[digit][0]) canvas.fillRectangle(xSegL, y, xSegR, y + t);
  // b: segment vertical dreapta-sus
  if (segMap[digit][1]) canvas.fillRectangle(x + w - t, yTopHalf, x + w, y + halfH);
  // c: segment vertical dreapta-jos
  if (segMap[digit][2]) canvas.fillRectangle(x + w - t, y + halfH + t, x + w, yBotHalf);
  // d: segment orizontal jos
  if (segMap[digit][3]) canvas.fillRectangle(xSegL, y + h - t, xSegR, y + h);
  // e: segment vertical stânga-jos
  if (segMap[digit][4]) canvas.fillRectangle(x, y + halfH + t, x + t, yBotHalf);
  // f: segment vertical stânga-sus
  if (segMap[digit][5]) canvas.fillRectangle(x, yTopHalf, x + t, y + halfH);
  // g: segment orizontal mijloc
  if (segMap[digit][6]) canvas.fillRectangle(xSegL, yMidTop, xSegR, yMidBot);
}

// Șterge zona unei cifre HH custom (pentru antiflicker)
void clear7Segment(int x, int y, int w, int h) {
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(x, y, x + w, y + h);
}

void setTimezone(String timezone){
  Serial.printf("  Setting Timezone to %s\n", timezone.c_str());
  setenv("TZ", timezone.c_str(), 1);
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
    case 57: return "Burnita inghetata densa";
    case 61: return "Ploaie usoara";
    case 63: return "Ploaie moderata";
    case 65: return "Ploaie abundenta";
    case 66: return "Ploaie inghetata usoara";
    case 67: return "Ploaie inghetata densa";
    case 71: return "Ninsoare usoara";
    case 73: return "Ninsoare moderata";
    case 75: return "Ninsoare abundenta";
    case 77: return "Graunte de zapada";
    case 80: return "Averse usoare";
    case 81: return "Averse moderate";
    case 82: return "Averse violente";
    case 85: return "Averse de zapada usoare";
    case 86: return "Averse de zapada abundente";
    case 95: return "Furtuna";
    case 96: return "Furtuna cu grindina mica";
    case 99: return "Furtuna cu grindina mare";
    default: return "Necunoscut";
  }
}

const char* directieVant(int grade) {
  static const char* directii[] = {
    "Nord", "Nord-Est", "Est", "Sud-Est",
    "Sud", "Sud-Vest", "Vest", "Nord-Vest"
  };
  int index = int((float)(grade / 45.)) % 8;
  return directii[index];
}

const char* descriereUV(float uv) {
  if (uv <= 2)  return "Scazut";
  if (uv <= 5)  return "Moderat";
  if (uv <= 7)  return "Ridicat";
  if (uv <= 10) return "Foarte ridicat";
  return "Extreme";
}

const char* descriereAQI(float aqi) {
  if (aqi <= 20)   return "Bun";
  if (aqi <= 40)   return "Acceptabil";
  if (aqi <= 60)   return "Moderat";
  if (aqi <= 80)   return "Slab";
  if (aqi <= 100)  return "Foarte slab";
  return "Extrem de slab";
}

// Parser JSON simplificat (fără ArduinoJson, economisește RAM)
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
  Serial.println(F("Fetching weather + AQ (Craiova)..."));

  // === API 1: Vremea curentă ===
  {
    WiFiClient client;
    client.setTimeout(8000);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (http.begin(client, weatherURL)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String payload = http.getString();

        String tempStr     = extractValue(payload, "temperature_2m");
        String humStr      = extractValue(payload, "relative_humidity_2m");
        String presStr     = extractValue(payload, "pressure_msl");
        String codeStr     = extractValue(payload, "weather_code");
        String windStr     = extractValue(payload, "wind_speed_10m");
        String windDirStr  = extractValue(payload, "wind_direction_10m");
        String uvStr       = extractValue(payload, "uv_index");
        String isDayStr    = extractValue(payload, "is_day");

        if (tempStr.length() > 0 && codeStr.length() > 0) {
          currentWeather.temperature = tempStr.toFloat();
          currentWeather.humidity    = humStr.toInt();
          currentWeather.pressure    = presStr.toFloat();
          currentWeather.weatherCode = codeStr.toInt();
          currentWeather.description = weatherCodeToDescription(currentWeather.weatherCode);
          currentWeather.windSpeed   = windStr.toFloat();
          currentWeather.windDir     = windDirStr.toInt();
          currentWeather.windDirDesc = directieVant(currentWeather.windDir);
          currentWeather.uvIndex     = uvStr.toFloat();
          currentWeather.uvDesc      = descriereUV(currentWeather.uvIndex);
          currentWeather.isDay       = isDayStr.toInt();
          currentWeather.valid       = true;
          currentWeather.dirty       = true;
          Serial.println(F("  -> Weather OK"));
        }
      } else {
        Serial.printf("  -> Weather FAILED (HTTP %d)\n", code);
      }
      http.end();
    }
  }

  // === API 2: Calitatea aerului ===
  {
    WiFiClient client2;
    client2.setTimeout(8000);
    HTTPClient http2;
    http2.setConnectTimeout(8000);
    http2.setTimeout(10000);

    if (http2.begin(client2, aqiURL)) {
      int code2 = http2.GET();
      if (code2 == HTTP_CODE_OK) {
        String payload2 = http2.getString();

        String aqiStr  = extractValue(payload2, "european_aqi");
        String pm25Str = extractValue(payload2, "pm2_5");
        String pm10Str = extractValue(payload2, "pm10");

        if (aqiStr.length() > 0) {
          currentWeather.euAqi   = aqiStr.toInt();
          currentWeather.aqiDesc = descriereAQI(currentWeather.euAqi);
          currentWeather.pm25    = pm25Str.toFloat();
          currentWeather.pm10    = pm10Str.toFloat();
          currentWeather.dirty   = true;
          Serial.println(F("  -> AQ OK"));
        }
      } else {
        Serial.printf("  -> AQ FAILED (HTTP %d)\n", code2);
      }
      http2.end();
    }
  }
}

// ============================================================
// FUNCȚII DE DESENARE - LAYOUT 640x388 (fabgl alocă 388 linii)
// ============================================================

// Zona 1 (Y=0-26): Ziua săptămânii + data
// (fără "sec:" si SS mic - au fost mutate sub MM)
void drawDate(const char* weekday_buf, const char* date_buf) {
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 0, 637, 26);

  canvas.selectFont(&fabgl::FONT_std_24);

  // Ziua săptămânii (alb, DoubleWidth)
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.setPenColor(Color::White);
  canvas.drawText(5, 1, weekday_buf);

  // Data (galben, fără DoubleWidth) - mutată la X=300 (avem loc fără SS)
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.setPenColor(Color::BrightYellow);
  canvas.drawText(300, 3, date_buf);
}

// Zona 3 (Y=58-340): HH custom + MM segment140 + SS sub M2 + puncte ":" pulsează
void drawClock(struct tm* timeinfo) {
  int h = timeinfo->tm_hour;
  int m = timeinfo->tm_min;
  int s = timeinfo->tm_sec;

  int h1 = h / 10, h2 = h % 10;
  int m1 = m / 10, m2 = m % 10;
  // SS desenat ca string "02d" (cifre apropiate, fără gap manual)

  // =======================================================
  // 1. SS sub M2 cu segment140 normal + bataia ":" (2 puncte primitive)
  // =======================================================
  if (s != prev_s) {
    // --- SS sub M2 (segment140 normal, fără DW) ---
    canvas.selectFont(&fabgl::FONT_Seven_Segment140);
    canvas.setGlyphOptions(
      GlyphOptions()
        .FillBackground(true)
        .DoubleWidth(false)
        .Bold(false)
        .Italic(false)
        .Underline(false)
    );

    int s1 = s / 10, s2 = s % 10;
    static int prev_s1 = -1, prev_s2 = -1;

    // La prima actualizare după prima desenare, forțăm sincronizarea
    // ca să nu redesenăm SS inutil
    if (prev_s1 == -1) { prev_s1 = s1; prev_s2 = s2; }

    if (s1 != prev_s1) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(S1_X, SS_Y, S1_X + DIGIT_W_NORM, SS_Y + SS_H);
      canvas.setPenColor(Color::BrightBlue);
      canvas.drawTextFmt(S1_X, SS_Y, "%d", s1);
    }
    if (s2 != prev_s2) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(S2_X, SS_Y, S2_X + DIGIT_W_NORM, SS_Y + SS_H);
      canvas.setPenColor(Color::BrightBlue);
      canvas.drawTextFmt(S2_X, SS_Y, "%d", s2);
    }
    prev_s1 = s1;
    prev_s2 = s2;

    // --- BATAIA: 2 puncte primitive care pulsează ---
    // (zona mică, controlată - nu atinge M1 la X=335)
    if (s % 2 == 0) {
      // Secundă pară: afișăm punctele (alb)
      canvas.setBrushColor(Color::White);
      canvas.fillRectangle(COL_DOT_X, COL_DOT_Y1, COL_DOT_X + COL_DOT_W, COL_DOT_Y1 + COL_DOT_H);
      canvas.fillRectangle(COL_DOT_X, COL_DOT_Y2, COL_DOT_X + COL_DOT_W, COL_DOT_Y2 + COL_DOT_H);
    } else {
      // Secundă impară: ștergem punctele (negru)
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(COL_DOT_X, COL_DOT_Y1, COL_DOT_X + COL_DOT_W, COL_DOT_Y1 + COL_DOT_H);
      canvas.fillRectangle(COL_DOT_X, COL_DOT_Y2, COL_DOT_X + COL_DOT_W, COL_DOT_Y2 + COL_DOT_H);
    }
  }

  // =======================================================
  // 2. HH:MM cu segment140 + DoubleWidth (140px înălțime, identic cu v.3.5)
  // =======================================================
  canvas.selectFont(&fabgl::FONT_Seven_Segment140);

  if (clock_first_draw) {
    // Prima desenare: ștergem toată zona ceasului și afișăm tot
    canvas.setBrushColor(Color::Black);
    canvas.fillRectangle(2, CLK_Y, 637, CLK_Y + CLK_H);  // 140px înălțime pt HH:MM
    canvas.fillRectangle(2, SS_Y, 637, SS_Y + SS_H);     // zona SS sub MM

    // HH (BrightRed) - DoubleWidth
    canvas.setGlyphOptions(
      GlyphOptions()
        .FillBackground(true)
        .DoubleWidth(true)
        .Bold(false)
        .Italic(false)
        .Underline(false)
    );
    canvas.setPenColor(Color::BrightRed);
    canvas.drawTextFmt(H1_X, CLK_Y, "%d", h1);
    canvas.drawTextFmt(H2_X, CLK_Y, "%d", h2);

    // MM (BrightYellow) - DoubleWidth
    canvas.setPenColor(Color::BrightYellow);
    canvas.drawTextFmt(M1_X, CLK_Y, "%d", m1);
    canvas.drawTextFmt(M2_X, CLK_Y, "%d", m2);

    // SS (BrightBlue) - segment140 normal (fără DW), sub M2
    canvas.setGlyphOptions(
      GlyphOptions()
        .FillBackground(true)
        .DoubleWidth(false)
        .Bold(false)
        .Italic(false)
        .Underline(false)
    );
    canvas.setPenColor(Color::BrightBlue);
    canvas.drawTextFmt(S1_X, SS_Y, "%d", s / 10);
    canvas.drawTextFmt(S2_X, SS_Y, "%d", s % 10);

    clock_first_draw = false;
  } else {
    // --- Actualizări incrementale HH:MM (segment140 + DoubleWidth) ---
    canvas.setGlyphOptions(
      GlyphOptions()
        .FillBackground(true)
        .DoubleWidth(true)
        .Bold(false)
        .Italic(false)
        .Underline(false)
    );

    if (h1 != prev_h1) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(H1_X, CLK_Y, H1_X + DIGIT_W_DW, CLK_Y + CLK_H);
      canvas.setPenColor(Color::BrightRed);
      canvas.drawTextFmt(H1_X, CLK_Y, "%d", h1);
    }
    if (h2 != prev_h2) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(H2_X, CLK_Y, H2_X + DIGIT_W_DW, CLK_Y + CLK_H);
      canvas.setPenColor(Color::BrightRed);
      canvas.drawTextFmt(H2_X, CLK_Y, "%d", h2);
    }
    if (m1 != prev_m1) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(M1_X, CLK_Y, M1_X + DIGIT_W_DW, CLK_Y + CLK_H);
      canvas.setPenColor(Color::BrightYellow);
      canvas.drawTextFmt(M1_X, CLK_Y, "%d", m1);
    }
    if (m2 != prev_m2) {
      canvas.setBrushColor(Color::Black);
      canvas.fillRectangle(M2_X, CLK_Y, M2_X + DIGIT_W_DW, CLK_Y + CLK_H);
      canvas.setPenColor(Color::BrightYellow);
      canvas.drawTextFmt(M2_X, CLK_Y, "%d", m2);
    }
  }

  prev_h1 = h1; prev_h2 = h2;
  prev_m1 = m1; prev_m2 = m2;
  prev_s  = s;
}

// Zonele 2, 4, 5, 6, 7: Toate informațiile meteo (rânduri separate)
void drawWeather() {
  if (!currentWeather.valid || !currentWeather.dirty) return;

  canvas.selectFont(&fabgl::FONT_std_24);
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(true)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );

  // ===== Zona 2 (Y=28-54): T+U+P (BrightGreen) =====
  // "um" = abreviere pentru umiditate (mai clar decât "U")
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 28, 637, 54);
  canvas.setPenColor(Color::BrightGreen);
  float pressureMmHg = currentWeather.pressure * 0.75006f;
  char line1[80];
  snprintf(line1, sizeof(line1), "T:%+.1f%cC  um:%d%%  P:%.0fmmHg",
           currentWeather.temperature, 0xB0,
           currentWeather.humidity, pressureMmHg);
  canvas.drawText(5, 30, line1);

  // ===== Zona 4 (Y=220-246): Vânt (BrightCyan) - coborât +20px =====
  // Fără spațiu între valoare și unitate (consistent cu T, P, PM)
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 220, 637, 246);
  canvas.setPenColor(Color::BrightCyan);
  char line2[80];
  snprintf(line2, sizeof(line2), "Vant: %s %.1fkm/h",
           currentWeather.windDirDesc.c_str(), currentWeather.windSpeed);
  canvas.drawText(5, 222, line2);

  // ===== Zona 5 (Y=248-274): Indice UV (BrightYellow) - coborât +20px =====
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 248, 637, 274);
  canvas.setPenColor(Color::BrightYellow);
  char line3[80];
  snprintf(line3, sizeof(line3), "UV: %.1f (%s)",
           currentWeather.uvIndex, currentWeather.uvDesc.c_str());
  canvas.drawText(5, 250, line3);

  // ===== Zona 6 (Y=276-302): Calitate aer AQI (BrightMagenta) - coborât +20px =====
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 276, 637, 302);
  canvas.setPenColor(Color::BrightMagenta);
  char line4[80];
  snprintf(line4, sizeof(line4), "Aer: %d (%s)",
           currentWeather.euAqi, currentWeather.aqiDesc.c_str());
  canvas.drawText(5, 278, line4);

  // ===== Zona 7 (Y=304-330): PM2.5 (BrightRed) - rând propriu =====
  // Spațiu după ":", µ (0xB5) în loc de u, fără spațiu între valoare și unitate
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 304, 637, 330);
  canvas.setPenColor(Color::BrightRed);
  char line5[80];
  snprintf(line5, sizeof(line5), "PM2.5: %.1f%cg/m3",
           currentWeather.pm25, 0xB5);
  canvas.drawText(5, 306, line5);

  // ===== Zona 8 (Y=332-358): PM10 (BrightRed) - sub PM2.5 =====
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 332, 637, 358);
  canvas.setPenColor(Color::BrightRed);
  char line6[80];
  snprintf(line6, sizeof(line6), "PM10: %.1f%cg/m3",
           currentWeather.pm10, 0xB5);
  canvas.drawText(5, 334, line6);

  // ===== Zona 9 (Y=360-386): Starea vremii (White) - coborât =====
  canvas.setBrushColor(Color::Black);
  canvas.fillRectangle(2, 360, 637, 386);
  canvas.setPenColor(Color::White);
  canvas.drawText(5, 362, currentWeather.description.c_str());

  currentWeather.dirty = false;
}

void printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }

  char weekday_buf[20], date_buf[40];
  snprintf(weekday_buf, sizeof(weekday_buf), "%s",
           getRomanianWeekday(timeinfo.tm_wday));
  snprintf(date_buf, sizeof(date_buf), "%02d %s %d",
           timeinfo.tm_mday,
           getRomanianMonth(timeinfo.tm_mon),
           timeinfo.tm_year + 1900);

  // Antiflicker: redesenăm doar la schimbare zi/dată
  if (strcmp(weekday_buf, lastWeekday) != 0 ||
      strcmp(date_buf,    lastDate)    != 0) {
    drawDate(weekday_buf, date_buf);
    strcpy(lastWeekday, weekday_buf);
    strcpy(lastDate,    date_buf);
  }

  drawClock(&timeinfo);
  drawWeather();
}

// ============================================================
// CHENAR DE TEST - pt a vedea marginea vizibilă a canvasului
// Desenează un chenar albastru de 2 pixeli pe toate cele 4 margini.
// Dacă nu vezi marginea de jos -> monitorul taie imaginea (overscan VGA).
// ============================================================
void drawBorder() {
  int w = canvas.getWidth();
  int h = canvas.getHeight();
  canvas.setBrushColor(Color::BrightBlue);
  // Marginea de sus (2px) - x incepe de la 0
  canvas.fillRectangle(0, 0,     w - 1, 1);
  // Marginea de jos (2px)
  canvas.fillRectangle(0, h - 2, w - 1, h - 1);
  // Marginea din stânga (2px) - x incepe de la 0
  canvas.fillRectangle(0, 0, 1, h - 1);
  // Marginea din dreapta (2px)
  canvas.fillRectangle(w - 2, 0, w - 1, h - 1);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
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

  // ===== INIT VGA 16 CULORI (4bpp) =====
  // VGA16Controller folosește 4 biți/pixel = 16 culori.
  // La 640x480 fabgl alocă automat doar 388 linii vizibile (limită RAM ESP32).
  // Layout-ul este dimensionat pentru 388 linii (max Y = 338).
  DisplayController.begin();
  DisplayController.setResolution(VGA_640x480_60Hz);

  canvas.setBrushColor(Color::Black);
  canvas.setGlyphOptions(
    GlyphOptions()
      .FillBackground(true)
      .DoubleWidth(false)
      .Bold(false)
      .Italic(false)
      .Underline(false)
  );
  canvas.clear();

  // Chenar de test eliminat (v.4.1) - nu mai e nevoie de el

  Serial.printf("[Canvas] Width=%d Height=%d\n",
                canvas.getWidth(), canvas.getHeight());
  Serial.printf("[Free heap after VGA init: %u bytes]\n", ESP.getFreeHeap());
  Serial.println(F("[MODE] 16 colors (4bpp) - VGA 640x388 (fabgl limited)"));

  fetchWeather();
  printLocalTime();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  static unsigned long lastWeatherFetch = 0;
  unsigned long now = millis();

  // Re-fetch meteo la fiecare 10 minute
  if (lastWeatherFetch == 0 ||
      (now - lastWeatherFetch) >= WEATHER_FETCH_INTERVAL) {
    fetchWeather();
    lastWeatherFetch = now;
  }

  printLocalTime();
  delay(1000);
}
