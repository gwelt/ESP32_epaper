// BUSY -> 4, RST -> 16, DC -> 17, CS -> SS(5), CLK -> SCK(18), DIN -> MOSI(23), GND -> GND, 3.3V -> 19 (3.3V)
#include <GxEPD.h>
#include <GxGDEW042T2/GxGDEW042T2.h>      // 4.2" b/w
#include <GxIO/GxIO_SPI/GxIO_SPI.h>
#include <GxIO/GxIO.h>
//#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
//#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
const GFXfont* f24 = &FreeMonoBold24pt7b;
const GFXfont* f12 = &FreeMonoBold12pt7b;
GxIO_Class io(SPI, /*CS=5*/ SS, /*DC=*/ 17, /*RST=*/ 16); // arbitrary selection of 17, 16
GxEPD_Class display(io, /*RST=*/ 16, /*BUSY=*/ 4); // arbitrary selection of (16), 4

#include "config.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
AsyncWebServer asyncServer(80);

#include <Preferences.h>
Preferences preferences;
String wifiSSID, wifiPassword;
bool rebootOnNoWiFi; // Should it reboot if no WiFi could be connected?

#include <SocketIOClient.h>
SocketIOClient sIOclient;
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
  Serial.begin(115200);
  display.init(115200);
  
  pinMode (19, OUTPUT); // 3.3v for e-paper-display
  digitalWrite (19, 1);
  epaper_init();

  pinMode(2, OUTPUT);
  blink(2,50);
  
  ++bootCount;
  //Serial.println("Boot number: " + String(bootCount));
  display.print("Boot #" + String(bootCount));
  switch(esp_sleep_get_wakeup_cause())
  {
    case 4 : display.println(" (timer)"); idle_counter=5; break;
    case 5 : display.println(" (touch)"); idle_counter=0; break;
    default : display.println(); break;
  }
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * 1000000ULL);  

  loadPreferences();
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_MODE_AP);
  setupAP();

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
    //display.setBrightness(0x03, true);
    epaper_init(); display.update();
  });
  asyncServer.on("/OFF", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    //display.setBrightness(0x00, false);
    epaper_init(); display.update();
  }); 
  asyncServer.on("/ART", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    art();
  }); 
  asyncServer.on("/TIME", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    sIOclient.send("broadcast","get","time");
  });
  asyncServer.on("/SCANNETWORKS", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", assembleRES());
    doScanNetworks();
  });
  asyncServer.on("/CONNECTWIFI", HTTP_GET, [](AsyncWebServerRequest *request){
    //connectWiFi();
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
    //restart=true;
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

  connectWiFi();
}


void loop(){
  if (deepsleep) {WiFi.disconnect(); delay(1000); goToDeepSleep();}
  if (restart) {WiFi.disconnect(); delay(1000); ESP.restart();}
  
  if (abs(millis()-timeflag)>10000) {
  	/* this is starting every 10 seconds */
    //Serial.print('.');
    timeflag = millis();
    
    if (idle_counter++>=MAX_IDLE_10_SECS) {deepsleep=true;} // go to sleep after MAX_IDLE_10_SECS*10 seconds idle-time

    if (sIOshouldBeConnected) {
      if (!sIOclient.connected()) {sIOclient.disconnect(); sIOshouldBeConnected=false;} 
      else {
        sIOclient.heartbeat(1); 
        if (ten_seconds_heartbeat_counter>=6) {
		  /* this is starting every minute */
          //sIOclient.send("broadcast","get","time");
          ten_seconds_heartbeat_counter=0;
        }
        ten_seconds_heartbeat_counter++;
      }
    }
    if (wifi_connected && !sIOshouldBeConnected) {
      blink(5,50); 
      connectSocketIO();
    }
  }

  if (sIOshouldBeConnected && sIOclient.monitor())
  {
    blink(1,50);
    //Serial.print(RID+", ");
    //Serial.print(Rname+", ");
    //Serial.println(Rcontent);
    if (Rname=="time") {epaper_update_time(Rcontent);} //{art_z=0; display.showNumberDecEx(Rcontent.toInt(), 0b01000000, true, 4, 0);}
    if (Rname=="number") {epaper_println(Rcontent);} //{art_z=0; display.showNumberDecEx(Rcontent.toInt(), 0b00000000, true, 4, 0);}
    if (Rname=="welcomemessage") {epaper_println(Rcontent); sIOclient.send("broadcast","get","time");} //Rcontent {art_z=0; art(12,440);}
    if (Rname=="") {connectSocketIO();}
    Rname="";
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
  //Serial.print("Stored SSID: ");
  //Serial.println(wifiSSID);
  //Serial.print("rebootOnNoWiFi: ");
  //Serial.println(String(rebootOnNoWiFi));
}

bool setupAP() {
  IPAddress AP_local_IP(8,8,8,8);
  IPAddress AP_gateway(8,8,8,8);
  IPAddress AP_subnet(255,255,255,0);
  WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet);
  delay(100);
  WiFi.softAP(AP_SSID);
  //Serial.print("Soft-AP SSID = ");
  //Serial.println(AP_SSID);
  //Serial.print("Soft-AP IP = ");
  //Serial.println(WiFi.softAPIP());
  //display.showNumberDecEx(8888, 0b00000000, false, 4, 0);
  display.print("8.8.8.8 @ "); display.println(AP_SSID);
  display.update();
}

bool connectWiFi() {
  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());  
  //Serial.println("Trying to connect to WiFi "+wifiSSID+".");
  int wait=10;
  while (WiFi.status() != WL_CONNECTED && wait>0) {wait--; delay(500); Serial.print("~");}
  if (WiFi.status()==WL_CONNECTED) {
    WiFi.mode(WIFI_MODE_APSTA);
    display.print(WiFi.localIP().toString()); display.print(" @ "); display.println(wifiSSID);
    display.update();
    //Serial.print("WiFi connected. IP address: ");
    //Serial.println(WiFi.localIP());
    //String ip=WiFi.localIP().toString(); ip=ip.substring(ip.lastIndexOf('.')+1,ip.length());
    //display.showNumberDecEx(ip.toInt(), 0b00000000, false, 4, 0);
    connectSocketIO();
    blink(1,800);
    return true;  
  } else {
    WiFi.mode(WIFI_MODE_AP);
    blink(2,150);
    display.println(wifiSSID + " connect failed");
    display.update();
    //Serial.print("Failed to connect WiFi. ");
    if (rebootOnNoWiFi) {
      //Serial.print("Will restart in 15 seconds..."); 
      delay(15000); ESP.restart();
    }
    return false;
  }
}

void connectSocketIO() {
  if (sIOshouldBeConnected) {sIOclient.disconnect();} 
  if (!sIOclient.connect(SOCKETIOHOST, SOCKETIOPORT)) {
    //Serial.println("Failed to connect to SocketIO-server "+String(SOCKETIOHOST));
  } 
  if (sIOclient.connected()) {
    //Serial.println("Connected to SocketIO-server "+String(SOCKETIOHOST));
    sIOshouldBeConnected=true;
    display.println(String(sIOclient.sid));
    display.update();
  } 
}

void doScanNetworks() {
  //Serial.println("Scan start ... ");
  int n = WiFi.scanNetworks();
  //Serial.print(n);
  display.print(n);
  //Serial.println(" network(s) found");
  display.println(" network(s) found");
  int i=0;
  while (i<n) {
    //Serial.println(WiFi.SSID(i));
    display.println(WiFi.SSID(i));
    i++;
  }
  display.update();
}

String assembleRES() {
  idle_counter=0;
  ++counter;
  //art(12,40);
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

void art() {
  epaper_history();
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
  display.println("SLEEPING > touch PIN");
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
  //Serial.println("Going to sleep now");
  delay(5000);
  esp_deep_sleep_start();
}

String urlDecode(const String& text)
{
  String decoded = "";
  char temp[] = "0x00";
  unsigned int len = text.length();
  unsigned int i = 0;
  while (i < len)
  {
    char decodedChar;
    char encodedChar = text.charAt(i++);
    if ((encodedChar == '%') && (i + 1 < len))
    {
      temp[2] = text.charAt(i++);
      temp[3] = text.charAt(i++);

      decodedChar = strtol(temp, NULL, 16);
    }
    else {
      if (encodedChar == '+')
      {
        decodedChar = ' ';
      }
      else {
        decodedChar = encodedChar;  // normal ascii char
      }
    }
    decoded += decodedChar;
  }
  return decoded;
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
      WiFi.mode(WIFI_MODE_APSTA);
      //Serial.println("STA Connected");
      //Serial.print("STA SSID: ");
      //Serial.println(WiFi.SSID());
      //Serial.print("STA IPv4: ");
      //Serial.println(WiFi.localIP());
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      wifi_connected = false;
      //Serial.println("WiFi disconnected");
      delay(10000);
      connectWiFi();
      break;
    default:
      break;
  }
}

void epaper_history()
{
  display.fillScreen(GxEPD_WHITE);
/*
  display.setFont(f24);
  display.setTextColor(GxEPD_BLACK);

  display.setCursor(100,120);
  display.print("ERSTER!");

  display.update();
  delay(300);
*/
  //uint8_t r = display.getRotation();
  //display.setRotation(r);
  //display.fillRect(display.width() - 18, 0, 16, 16, GxEPD_BLACK);
  //display.fillRect(display.width() - 25, display.height() - 25, 24, 24, GxEPD_BLACK);
  display.fillRect(0, 60, display.width(), 100, GxEPD_BLACK);

  display.setFont(f24);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(35,120);
  display.print("Hello World!");

  display.setFont(f12);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(45,200);
  display.println("So faengt es immer an."); display.println();

  display.update();
  //delay(3000);
}

void epaper_init()
{
  display.fillScreen(GxEPD_WHITE);
  display.setFont(f12);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(0,20);
}

void epaper_update_time(const String& text)
{
  display.println(text);
  display.update();
}

void epaper_println(const String& text)
{
  display.println(text);
  display.update();
}

