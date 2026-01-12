#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <StreamUtils.h>
#include "time.h"
#include "esp_sntp.h"

#include "EPD_4in0e.h"
#include "GUI_Paint.h"

#include <ArduinoJson.h>
#include <math.h>

#include "secrets.h"

// from https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TZ.h
#define TZ_America_Chicago PSTR("CST6CDT,M3.2.0,M11.1.0")


const char *lat_lon = "42.1057,-88.0585";
const char *excluded = "minutely,alerts";

const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";

enum class ICON {
  ClearDay, ClearNight, Thunderstorm, Rain, Snow, Sleet, Wind, Fog, Cloudy, PartlyCloudyDay, PartlyCloudyNight
};

enum class PRECIP {
  Rain, Snow, Sleet, Ice, Mixed, None
};

struct Daily {
  ICON icon;
  PRECIP precipType;
  int tempHigh;
  int tempLow;
  int precipProb;
};

const int DAYS = 3;
Daily dayInfo[DAYS];

struct Hourly {
  // ICON icon;
  PRECIP precipType;
  int precipProb;
};

Hourly hourlyInfo[12];

timeval* timeAcquired = 0;
struct tm timeinfo;

#define uS_TO_S_FACTOR 1000000ULL  // Conversion factor for micro seconds to seconds

RTC_DATA_ATTR int bootCount = 0;

void timeavailable(struct timeval *t) {
  timeAcquired = t;
  if(!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get local time");
  }
}

void setup() {
  unsigned long start = millis();
  bootCount++;
  // put your setup code here, to run once:
  WiFi.begin(ssid, password);
  DEV_Module_Init();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED");

  // set notification call-back function
  sntp_set_time_sync_notification_cb(timeavailable);
  configTzTime(TZ_America_Chicago, ntpServer1, ntpServer2);
  while(!timeAcquired) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" TIME SYNCED");
  esp_sntp_stop();

  getWeather();
  getHourly();
  WiFi.disconnect(true);

  UBYTE *Image = (UBYTE*)malloc((400 * 600) / 2);
  if(Image == NULL) {
    Serial.println("unable to allocate memory for image");
  }
  Paint_NewImage(Image, EPD_4IN0E_WIDTH, EPD_4IN0E_HEIGHT, 90, EPD_4IN0E_WHITE);
  Paint_SelectImage(Image);
  Paint_SetScale(6);
  Paint_Clear(EPD_4IN0E_WHITE);

  EPD_4IN0E_Init();
  EPD_4IN0E_Clear(EPD_4IN0E_WHITE);

  drawDate();
  drawWeather();
  EPD_4IN0E_DisplayPart(Image, 0, 0, 400, 600);
  EPD_4IN0E_Sleep();

  // use the time taken since boot to try and keep the refresh aligned to 6 hours
  unsigned long end = millis();
  esp_err_t res = esp_sleep_enable_timer_wakeup((uS_TO_S_FACTOR * 60 * 60 * 1) - ((end-start) * 1000));
  if(res == ESP_ERR_INVALID_ARG) {
    Serial.print("Invalid sleep time");
  }
  esp_deep_sleep_start();
}

void getWeather() {
  WiFiClientSecure cs;
  cs.setInsecure();
  // set timeout to 10 seconds
  cs.setTimeout(10000);
  HTTPClient http;
  http.useHTTP10(true);
  // Ask HTTPClient to collect the Transfer-Encoding header
  // (by default, it discards all headers)
  const char* keys[] = {"Transfer-Encoding", "Content-Length"};
  http.collectHeaders(keys, 2);
  http.setReuse(false);

  JsonDocument filter;

  JsonObject filter_currently = filter["currently"].to<JsonObject>();
  filter_currently["icon"] = true;
  filter_currently["temperature"] = true;

  JsonObject filter_hourly_data_0 = filter["hourly"]["data"].add<JsonObject>();
  // filter_hourly_data_0["icon"] = true;
  filter_hourly_data_0["precipType"] = true;
  filter_hourly_data_0["precipProbability"] = true;

  JsonObject filter_daily_data_0 = filter["daily"]["data"].add<JsonObject>();
  filter_daily_data_0["icon"] = true;
  filter_daily_data_0["precipType"] = true;
  filter_daily_data_0["precipProbability"] = true;
  filter_daily_data_0["temperatureHigh"] = true;
  filter_daily_data_0["temperatureLow"] = true;

  JsonDocument doc;

  int code;
  char** url;
  asprintf(url, "https://api.pirateweather.net/forecast/%s/42.1057,-88.0585?exclude=minutely,hourly,alerts", api_key);
  if(http.begin(cs, *url)){
    code = http.GET();
    Serial.printf("HTTP Response code %d \n", code);
  }else{
    Serial.println("Failed to construct http request");
  }

  // decorate the WifiClient so it logs its content to Serial
  ReadLoggingStream loggingStream(http.getStream(), Serial);  

  // Choose the right stream depending on the Transfer-Encoding header
  Serial.println(http.header("Transfer-Encoding"));
  Serial.println(http.header("Content-Length"));
  // Stream& response =
  //     http.header("Transfer-Encoding") == "chunked" ? decodedStream : wifiClientWithLog;
  DeserializationError error = deserializeJson(doc, loggingStream, DeserializationOption::Filter(filter));

  if (error) {
    Serial.println();
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  const char* currently_icon = doc["currently"]["icon"]; // "partly-cloudy-night"
  float currently_temperature = doc["currently"]["temperature"]; // 29.86
  int cTemp = (int)(round(currently_temperature));

  int day = 0;
  for (JsonObject daily_data_item : doc["daily"]["data"].as<JsonArray>()) {
    if(day == DAYS) {
      break;
    } 
    const char* daily_data_item_icon = daily_data_item["icon"]; // "wind", "wind", "partly-cloudy-day", ...
    dayInfo[day].icon = parseIcon(daily_data_item_icon);
    float daily_data_item_precipProbability = daily_data_item["precipProbability"]; // 0.08, 0, 0, 0.21, ...
    dayInfo[day].precipProb = (int)(round(100.0*daily_data_item_precipProbability));
    const char* daily_data_item_precipType = daily_data_item["precipType"]; // "snow", "snow", "rain", ...
    dayInfo[day].precipType = parsePercip(daily_data_item_precipType);
    float daily_data_item_temperatureHigh = daily_data_item["temperatureHigh"]; // 36.91, 33.24, 41.41, ...
    dayInfo[day].tempHigh = (int)(round(daily_data_item_temperatureHigh));
    float daily_data_item_temperatureLow = daily_data_item["temperatureLow"]; // 23.11, 28.63, 33.46, 33.85, ...
    dayInfo[day].tempLow = (int)(round(daily_data_item_temperatureLow));
    day++;
  }
  http.end();
}

void getHourly(){
  WiFiClientSecure cs;
  cs.setInsecure();
  // set timeout to 10 seconds
  cs.setTimeout(10000);
  HTTPClient http;
  http.useHTTP10(true);
  // Ask HTTPClient to collect the Transfer-Encoding header
  // (by default, it discards all headers)
  const char* keys[] = {"Transfer-Encoding", "Content-Length"};
  http.collectHeaders(keys, 2);
  http.setReuse(false);

  JsonDocument filter;

  JsonObject filter_currently = filter["currently"].to<JsonObject>();
  filter_currently["icon"] = true;
  filter_currently["temperature"] = true;

  JsonObject filter_hourly_data_0 = filter["hourly"]["data"].add<JsonObject>();
  // filter_hourly_data_0["icon"] = true;
  filter_hourly_data_0["precipType"] = true;
  filter_hourly_data_0["precipProbability"] = true;

  JsonObject filter_daily_data_0 = filter["daily"]["data"].add<JsonObject>();
  filter_daily_data_0["icon"] = true;
  filter_daily_data_0["precipType"] = true;
  filter_daily_data_0["precipProbability"] = true;
  filter_daily_data_0["temperatureHigh"] = true;
  filter_daily_data_0["temperatureLow"] = true;

  JsonDocument doc;
  int code;
  if(http.begin(cs, "https://api.pirateweather.net/forecast/0jVBJfDQ9DYALRas4pc47nK5euzioIoh/42.1057,-88.0585?exclude=minutely,currently,daily,alerts")){
    code = http.GET();
    Serial.printf("HTTP Response code %d \n", code);
  }else{
    Serial.println("Failed to construct http request");
  }

  // decorate the WifiClient so it logs its content to Serial
  ReadLoggingStream loggingStream(http.getStream(), Serial);  

  // Choose the right stream depending on the Transfer-Encoding header
  Serial.println(http.header("Transfer-Encoding"));
  Serial.println(http.header("Content-Length"));
  // Stream& response =
  //     http.header("Transfer-Encoding") == "chunked" ? decodedStream : wifiClientWithLog;
  DeserializationError error = deserializeJson(doc, loggingStream, DeserializationOption::Filter(filter));

  if (error) {
    Serial.println();
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  int hour = 0;
  for (JsonObject hourly_data_item : doc["hourly"]["data"].as<JsonArray>()) {
    if(hour == 12) {
      break;
    }
    // const char* hourly_data_item_icon = hourly_data_item["icon"]; // "partly-cloudy-night", "wind", ...
    float hourly_data_item_precipProbability = hourly_data_item["precipProbability"]; // 0.08, 0.06, 0, 0, ...
    int prob = (int)(round(100.0* hourly_data_item_precipProbability));
    const char* hourly_data_item_precipType = hourly_data_item["precipType"]; // "snow", "snow", "snow", ...
    hour++;
  }
  http.end();
}

PRECIP parsePercip(const char* precipStr) {
  switch(precipStr[0]){
      case 'r': //rain
      return PRECIP::Rain;
      case 'i': // ice
      return PRECIP::Ice;
      case 'm':
      return PRECIP::Mixed;
      case 's':
      if(precipStr[1] == 'n') {
        return PRECIP::Snow;
      }else {
        return PRECIP::Sleet;
      }
      case 'n':
      default:
      return PRECIP::None;
  }
}

ICON parseIcon(const char *iconStr) {
  switch(iconStr[0]){
    case 'c': // clear-day, clear-night, cloudy
    if(iconStr[3] == 'o'){
      return ICON::Cloudy;
    }else {
      return ICON::ClearDay;
    }
    case 'p': // partly-cloudy-day, partly-cloudy-night
    if(iconStr[14] == 'd'){
      return ICON::PartlyCloudyDay;
    }else {
      return ICON::PartlyCloudyDay;
    }
    case 'w': // wind
    return ICON::Wind;
    case 't': // thunderstorm
    return ICON::Thunderstorm;
    case 'r': // rain
    return ICON::Rain;
    case 's': // snow, sleet
    if(iconStr[1] == 'n'){
      return ICON::Snow;
    }else {
      return ICON::Sleet;
    }
    case 'f': // fog
    return ICON::Fog;
    default:
    return ICON::Cloudy;
  }
}

const char months[12][4] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char weekday[7][4]  = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

void drawDate(){
  char dayOfMonth[3];

  sprintf(dayOfMonth, "%02d", timeinfo.tm_mday);
  Paint_DrawString_EN(10, 10, weekday[timeinfo.tm_wday], &Font16, EPD_4IN0E_WHITE, EPD_4IN0E_BLACK, 3);
  Paint_DrawString_EN(115, 10, months[timeinfo.tm_mon], &Font16,  EPD_4IN0E_WHITE, EPD_4IN0E_BLACK, 3);
  Paint_DrawString_EN(10, 62, dayOfMonth, &Font24, EPD_4IN0E_WHITE, EPD_4IN0E_BLACK,  5);
}

void drawWeather(){
  drawCurrent();
  drawHourly();
  drawDaily();
}

void drawCurrent(){

}

void drawHourly(){

}

void drawDaily(){
  char temp[3];
  for(int i = 0; i < DAYS; i++) {
    sprintf(temp, "%2d", dayInfo[i].tempHigh);
    Paint_DrawString_EN(200 + (i * 100), 200, temp, &Font24, EPD_4IN0E_WHITE, EPD_4IN0E_RED, 2);
    sprintf(temp, "%2d", dayInfo[i].tempLow);
    Paint_DrawString_EN(200 + (i * 100), 350, temp, &Font24, EPD_4IN0E_WHITE, EPD_4IN0E_BLUE, 2);
  }
}

void loop() {
  // put your main code here, to run repeatedly:

}
