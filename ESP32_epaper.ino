#include "config.h"
// BUSY -> 4, RST -> 16, DC -> 17, CS -> SS(5), CLK -> SCK(18), DIN -> MOSI(23), GND -> GND, 3.3V -> 19 (3.3V)
#include <GxEPD.h>
#include <GxGDEW042T2/GxGDEW042T2.h> // 4.2" b/w
#include <GxIO/GxIO_SPI/GxIO_SPI.h>
#include <GxIO/GxIO.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
const GFXfont* f9 = &FreeMonoBold9pt7b;
const GFXfont* f12 = &FreeMonoBold12pt7b;
const GFXfont* f24 = &FreeMonoBold24pt7b;
GxIO_Class io(SPI, /*CS=5*/ SS, /*DC=*/ 17, /*RST=*/ 16); // arbitrary selection of 17, 16
GxEPD_Class display(io, /*RST=*/ 16, /*BUSY=*/ 4); // arbitrary selection of (16), 4
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
AsyncWebServer asyncServer(80);

#include <Preferences.h>
Preferences preferences;
String wifiSSID, wifiPassword;
bool rebootOnNoWiFi; // Should it reboot if no WiFi could be connected?

#include <SocketIOClient.h>
SocketIOClient sIOclient;
extern String R;
extern String RID;
extern String Rname;
extern String Rcontent;

#define ESP32
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int counter = 0; // current counter
unsigned long timeflag = 0; // millis at last update
static volatile bool wifi_connected = false;
static volatile bool sIOshouldBeConnected=false;
static volatile bool deepsleep=false;
static volatile bool restart=false;

int ten_seconds_heartbeat_counter=6;
int idle_counter=0;

void setup()
{
  pinMode(2, OUTPUT);
  blink(1,1000);

  Serial.begin(115200);
  display.init(115200);

  pinMode (19, OUTPUT); // 3.3v for e-paper
  digitalWrite (19, 1);
  epaper_init();

  ++bootCount;
  switch(esp_sleep_get_wakeup_cause())
  {
    case 4 : epaper_print_status1("Boot #" + String(bootCount)+" (timer)"); idle_counter=MAX_IDLE_10_SECS-1; break; /* just 10 seconds till next sleep if timer caused wake-up */
    case 5 : epaper_print_status1("Boot #" + String(bootCount)+" (touch)"); break;
    default : epaper_print_status1("Boot #" + String(bootCount)+" (start)"); break;
  }
  epaper_message();

  loadPreferences();
  connectWiFi();

  asyncServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
  });
  asyncServer.on("/H", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    digitalWrite(2, HIGH); 
  });
  asyncServer.on("/L", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    digitalWrite(2, LOW); 
  });
  asyncServer.on("/ON", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    epaper_init();
  });
  asyncServer.on("/OFF", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    epaper_init();
  }); 
  asyncServer.on("/ART", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    epaper_message();
  }); 
  asyncServer.on("/TIME", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    //sIOclient.send("broadcast","get","time");
    sIOclient.send("message","time");
  });
  asyncServer.on("/SCANNETWORKS", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    doScanNetworks();
  });
  asyncServer.on("/CONNECTWIFI", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    restart=true;
  });
  asyncServer.on("/RESTART", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    restart=true;
  });
  asyncServer.on("/CONNECTSOCKET", HTTP_GET, [](AsyncWebServerRequest *request){
    connectSocketIO();
    request->send(200, "text/html", assembleRES());
  });
  asyncServer.on("/SLEEP", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    deepsleep=true;
  });
  
  asyncServer.on("/conf", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasArg("ssid") && request->hasArg("pass")) {
      savePreferences(request->arg("ssid"),request->arg("pass"),(request->arg("rebootOnNoWiFi")=="on"));
      request->send(200, "text/html", request->arg("ssid")+" "+request->arg("pass")+" "+String(request->arg("rebootOnNoWiFi")=="on"));
    } else {
      request->send(200, "text/html", assembleRES());
    }
  }); 

  asyncServer.begin();
}


void loop(){
  if (deepsleep) {WiFi.disconnect(); esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * 1000000ULL); delay(1000); goToDeepSleep();}
  if (restart) {WiFi.disconnect(); delay(1000); ESP.restart();}
  
  if (abs(millis()-timeflag)>10000) {
  	/* this is starting every 10 seconds */
    timeflag = millis();
    
    if (idle_counter++>=MAX_IDLE_10_SECS) {deepsleep=true;} // go to sleep after MAX_IDLE_10_SECS*10 seconds idle-time

    if (sIOshouldBeConnected) {
      if (!sIOclient.connected()) {sIOclient.disconnect(); sIOshouldBeConnected=false;} 
      else {
        sIOclient.heartbeat(1); 
        if (ten_seconds_heartbeat_counter>=6) {
		      /* this is starting every minute */
          ten_seconds_heartbeat_counter=0;
        }
        ten_seconds_heartbeat_counter++;
      }
    }
    if (wifi_connected && !sIOshouldBeConnected) {
      connectSocketIO();
    }
  }

  if (sIOshouldBeConnected && sIOclient.monitor())
  {
    blink(1,50);
    idle_counter=0;
    epaper_print(R.substring(R.indexOf("\",") + 3, R.indexOf("\"\]")));
    //sIOclient.send("message","time");
    /*
    if (Rname=="time") {epaper_print(Rcontent);}
    if (Rname=="number") {epaper_print(Rcontent);}
    if (Rname=="welcomemessage") {epaper_print(Rcontent); sIOclient.send("broadcast","get","time");}
    if (Rname=="") {connectSocketIO();}
    Rname="";
    */
  }
}

bool savePreferences(String qsid, String qpass, bool rebootOnNoWiFi) {
  // Remove all preferences under opened namespace
  preferences.clear();
  preferences.begin("wifi", false);
  preferences.putString("ssid", qsid);
  preferences.putString("password", qpass);
  preferences.putBool("rebootOnNoWiFi", rebootOnNoWiFi);
  delay(300);
  preferences.end();
  wifiSSID = qsid;
  wifiPassword = qpass;
  loadPreferences();
}

bool loadPreferences() {
  // Remove all preferences under opened namespace
  preferences.clear();
  preferences.begin("wifi", false);
  wifiSSID =  preferences.getString("ssid", "none");
  wifiPassword =  preferences.getString("password", "none");
  rebootOnNoWiFi =  preferences.getBool("rebootOnNoWiFi", false);
  preferences.end();
}

bool setupAP() {
  WiFi.mode(WIFI_MODE_AP);
  IPAddress AP_local_IP(8,8,8,8);
  IPAddress AP_gateway(8,8,8,8);
  IPAddress AP_subnet(255,255,255,0);
  WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet);
  delay(100);
  WiFi.softAP(AP_SSID);
  epaper_print_status2("8.8.8.8 @ " + String(AP_SSID));
}

bool connectWiFi() {
  //epaper_print_status2("connecting " + String(wifiSSID));
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());  
  int wait=10;
  while (WiFi.status() != WL_CONNECTED && wait>0) {wait--; delay(500);} //Serial.print("~");
  if (WiFi.status()==WL_CONNECTED) {
    //WiFi.mode(WIFI_MODE_APSTA);
    epaper_print_status2(WiFi.localIP().toString() + " @ " + wifiSSID);
    connectSocketIO();
    return true;  
  } else {
    //WiFi.mode(WIFI_MODE_AP);
    epaper_print_status2(wifiSSID + " connect failed");
    setupAP();
    if (rebootOnNoWiFi) {
      epaper_print("restarting in 15 seconds...");
      delay(15000); ESP.restart();
    }
    return false;
  }
}

void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_AP_START:
      //WiFi.softAPsetHostname(AP_SSID);
      break;
    case SYSTEM_EVENT_STA_START:
      //WiFi.setHostname(AP_SSID);
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      wifi_connected = true;
      //WiFi.mode(WIFI_MODE_APSTA);
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      wifi_connected = false;
      //delay(10000);
      //connectWiFi();
      break;
    default:
      break;
  }
}

void connectSocketIO() {
  if (sIOshouldBeConnected) {sIOclient.disconnect();} 
  if (!sIOclient.connect(SOCKETIOHOST, SOCKETIOPORT)) {
    epaper_print(String(SOCKETIOHOST) + " socket failed");
  } 
  if (sIOclient.connected()) {
    sIOshouldBeConnected=true;
    //epaper_print("HELLO " + String(sIOclient.sid));
    //sIOclient.send("message","/nick ESP32");
    sIOclient.send("message","/repeat");
  } 
}

void doScanNetworks() {
  int n = WiFi.scanNetworks();
  display.fillScreen(GxEPD_WHITE);
  display.setFont(f12);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0,20);
  display.print(n);
  display.println(" network(s) found");
  int i=0;
  while (i<n) {
    display.println(WiFi.SSID(i));
    i++;
  }
  display.update();
}

String assembleRES() {
  idle_counter=0;
  ++counter;
  blink(1,50);
  String sid="not connected";
  if (sIOshouldBeConnected) {sid=sIOclient.sid;}
  return "<html><head><title>ESP32-Cube</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body style=font-size:1.5em><a href=\"/\">ESP32-Cube</a> (<a href=https://github.com/urbaninnovation/ESP32-Cube>GitHub</a>)<br>LED <a href=\"/H\">ON</a> | <a href=\"/L\">OFF</a><br>DISPLAY <a href=\"/ON\">ON</a> | <a href=\"/OFF\">OFF</a><br><a href=\"/ART\">START DISPLAY ART</a><br><a href=\"/SLEEP\">DEEP SLEEP</a> ("
  +String(bootCount)
  +")<br><a href=\"/RESTART\">RESTART</a><br><a href=\"/SCANNETWORKS\">SCAN NETWORKS</a><br><a href=\"/CONNECTWIFI\">CONNECT TO WIFI</a><br><a href=\"/CONNECTSOCKET\">CONNECT TO SERVER</a><br><a href=\"/TIME\">REQUEST TIME</a><br>SID: "
  +String(sid)
  +"<br>AP SSID: "
  +AP_SSID
  +"<br>WiFi: "
  +wifiSSID
  +"<br>IP: "
  +WiFi.localIP().toString()
  +"<br>rebootOnNoWiFi: "
  +String(rebootOnNoWiFi)
  +"<br>COUNTER: "
  +String(counter)
  +"<form method='post' action='conf'><label></label><input name='ssid' style=width:20%><input name='pass' style=width:20%><input type='checkbox' name='rebootOnNoWiFi'><label for='rebootOnNoWiFi'>rebootOnNoWiFi</label><input type='submit'></form>"
  +"</body></html>";
}

void blink(int z, int d) {
  for (int i=0; i < z; i++){
    digitalWrite(2, !digitalRead(2));
    delay(d);
    digitalWrite(2, !digitalRead(2));
    delay(d);
  }
}

void goToDeepSleep() {
  epaper_print_status2("SLEEPING " + String(TIME_TO_SLEEP) + " > touch PIN");
  display.update();
  //#define BUTTON_PIN_BITMASK 0x200000000 // 2^33 in hex
  //DEEP SLEEP while PIN 33 is connected to GND //or while touchsensor on PIN15 isn't touched
  ////DEEP SLEEP while PIN 33 is connected to GND
  //esp_err_t rtc_gpio_deinit(GPIO_NUM_33);
  //esp_err_t rtc_gpio_pullup_en(GPIO_NUM_33);
  //esp_sleep_enable_ext0_wakeup(GPIO_NUM_33,0); //1 = High, 0 = Low
  ////or while touchsensor on PIN15 isn't touched
  touchAttachInterrupt(T3, {}, 60); // T3=PIN15, Threshold=40
  esp_sleep_enable_touchpad_wakeup();
  digitalWrite(2, true); 
  delay(1000);
  esp_deep_sleep_start();
}

void epaper_init()
{
  display.fillScreen(GxEPD_WHITE);
  display.setFont(f12);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0,20);
  display.update();
}

void epaper_message()
{
  display.updateWindow(0, 0, GxEPD_WIDTH, GxEPD_HEIGHT, false);
  display.fillRect(0, 0, GxEPD_WIDTH, 100, GxEPD_BLACK);
  display.setFont(f24);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(30,60);
  display.print("Hello World!");
  display.updateWindow(0, 0, GxEPD_WIDTH, 100, true);
}

void epaper_print(const String& text) {epaper_update(0, 105, GxEPD_WIDTH, 120, text, false);}
void epaper_print_status1(const String& text) {epaper_update(0, GxEPD_HEIGHT-60, GxEPD_WIDTH, 30, text, false);}
void epaper_print_status2(const String& text) {epaper_update(0, GxEPD_HEIGHT-30, GxEPD_WIDTH, 30, text, false);}

void epaper_update(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const String& text, bool invert)
{
  display.updateWindow(0, 0, GxEPD_WIDTH, GxEPD_HEIGHT, false);

  display.fillRect(x, y, w, h, GxEPD_BLACK);
  display.updateWindow(x, y, w, h, true);

  display.fillRect(x, y, w, h, GxEPD_WHITE);
  display.updateWindow(x, y, w, h, true);

  display.fillRect(x, y, w, h, GxEPD_WHITE);
  display.setCursor(x,y+22);
  display.setFont(f12);
  display.setTextColor(GxEPD_BLACK);
  display.println(text);
  display.updateWindow(x, y, w, h, true);
}


