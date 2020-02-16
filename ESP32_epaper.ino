#include "config.h"
// BUSY -> GPIO 4, RST -> GPIO 16, DC -> GPIO 17, CS -> SS(GPIO 5), CLK -> SCK(GPIO 18), DIN -> MOSI(GPIO 23), GND -> GND, 3.3V -> GPIO 19 (3.3V), TOUCH PIN -> GPIO 15
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
String pref_wifiSSID, pref_wifiPassword;
uint32_t pref_time_to_sleep = 900;
uint32_t pref_max_idle_secs = 60;

#include <SocketIOClient.h>
SocketIOClient sIOclient;
extern String R;

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int counter = 0; // current counter
unsigned long timeflag = 0; // millis at last update
static volatile bool wifi_connected = false;
static volatile bool sIO_connected=false;
static volatile bool deepsleep=false;
static volatile bool restart=false;

int last_action=millis();

void setup()
{
	pinMode(2, OUTPUT);
	blink(1,250);

	Serial.begin(115200);
	display.init(115200);

	pinMode (19, OUTPUT); // 3.3v for e-paper
	digitalWrite (19, 1);

	epaper_init();

	loadPreferences();

	++bootCount;
	switch(esp_sleep_get_wakeup_cause())
	{
		case 4 : epaper_print_status1("Boot #" + String(bootCount)+" (timer)"); deepsleep=true; break;
		case 5 : epaper_print_status1("Boot #" + String(bootCount)+" (touch)"); break;
		default : epaper_print_status1("Boot #" + String(bootCount)+" (start)"); break;
	}

	connectWiFi();
	startHTTPserver();

	epaper_message();
}

void loop(){
	if (deepsleep) {WiFi.disconnect(); esp_sleep_enable_timer_wakeup(pref_time_to_sleep * 1000000ULL); delay(1000); goToDeepSleep();}
	if (restart) {WiFi.disconnect(); delay(1000); ESP.restart();}
	
	if (abs(millis()-timeflag)>10000) {
		/* this is starting every 10 seconds - and at the very beginning */
		timeflag=millis();
		
		if (timeflag-last_action>=pref_max_idle_secs*1000) {deepsleep=true;} // go to sleep after pref_max_idle_secs seconds idle-time

		if (sIO_connected) {
			if (!sIOclient.connected()) {sIOclient.disconnect(); sIO_connected=false;} 
			else {sIOclient.heartbeat(1);}
		}
		if (wifi_connected && !sIO_connected) {
			connectSocketIO();
		}

	}

	if (sIO_connected && sIOclient.monitor())
	{
		blink(1,50);
		//last_action=millis();
		epaper_print(R.substring(R.indexOf("\",") + 3, R.indexOf("\"\]")));
	}
}

bool savePreferences(String qsid, String qpass, uint32_t qtime_to_sleep, uint32_t qmax_idle_secs) {
	preferences.begin("pref", false);
	// Remove all preferences under opened namespace
	preferences.clear();
	preferences.putString("ssid", qsid);
	preferences.putString("password", (qpass!="")?qpass:pref_wifiPassword);
	preferences.putUInt("max_idle_secs", qmax_idle_secs);
	preferences.putUInt("time_to_sleep", qtime_to_sleep);
	preferences.end();
	delay(300);
	loadPreferences();
}

bool loadPreferences() {
	preferences.begin("pref", false);
	pref_wifiSSID =  preferences.getString("ssid", "");
	pref_wifiPassword = preferences.getString("password", "");
	pref_max_idle_secs = preferences.getUInt("max_idle_secs", pref_max_idle_secs);
	pref_time_to_sleep = preferences.getUInt("time_to_sleep", pref_time_to_sleep);
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
	WiFi.onEvent(WiFiEvent);
	WiFi.mode(WIFI_MODE_APSTA);
	WiFi.begin(pref_wifiSSID.c_str(), pref_wifiPassword.c_str());
	int wait=10;
	while (WiFi.status() != WL_CONNECTED && wait>0) {wait--; delay(500);}
	if (WiFi.status()==WL_CONNECTED) {
		WiFi.mode(WIFI_MODE_STA);
		epaper_print_status2(WiFi.localIP().toString() + " @ " + WiFi.SSID());
		return true;  
	} else {
		epaper_print_status2(pref_wifiSSID + " connect failed");
		setupAP();
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
	if (sIO_connected) {sIOclient.disconnect();} 
	if (!sIOclient.connect(SOCKETIOHOST, SOCKETIOPORT)) {
		epaper_print(String(SOCKETIOHOST) + " socket failed");
	} 
	if (sIOclient.connected()) {
		sIO_connected=true;
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

void startHTTPserver() {
	asyncServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES(""));
	});
	asyncServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", "");
	});
	asyncServer.on("/H", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("LED ON"));
		digitalWrite(2, HIGH); 
	});
	asyncServer.on("/L", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("LED OFF"));
		digitalWrite(2, LOW); 
	});
	asyncServer.on("/DISPLAYINIT", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("DISPLAYINIT"));
		epaper_init();
	});
	asyncServer.on("/MILLIS", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("MILLIS"));
		epaper_print("MILLIS = "+String(millis()));
	}); 
	asyncServer.on("/TIME", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("TIME"));
		//sIOclient.send("broadcast","get","time");
		sIOclient.send("message","time");
	});
	asyncServer.on("/SCANNETWORKS", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("SCANNETWORKS"));
		doScanNetworks();
	});
	asyncServer.on("/RESTART", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("restarting..."));
		restart=true;
	});
	asyncServer.on("/SLEEP", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", assembleRES("going to sleep..."));
		delay(2000);
		deepsleep=true;
	});
	
	asyncServer.on("/conf", HTTP_POST, [](AsyncWebServerRequest *request){
		if (request->hasArg("ssid") && request->hasArg("pass") && request->hasArg("time_to_sleep") && request->hasArg("max_idle_secs")) {
			savePreferences(request->arg("ssid"),request->arg("pass"),request->arg("time_to_sleep").toInt(),request->arg("max_idle_secs").toInt());
			delay(500);
			request->send(200, "text/html", assembleRES("config saved"));
		} else {
			request->send(200, "text/html", assembleRES("did not write config"));
		}
	}); 

	asyncServer.begin();
}

String assembleRES(String message) {
	last_action=millis();
	++counter;
	blink(1,50);
	String address=wifi_connected?"<a href=\"/\">"+WiFi.localIP().toString()+ "</a> @ " +WiFi.SSID():"<a href=\"/\">8.8.8.8</a> @ " + String(AP_SSID);
	return "<!DOCTYPE html><html lang=\"de\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><meta name=\"theme-color\" content=\"#FFF\"><meta name=\"Description\" content=\"ESP32\"><title>ESP32</title><style>* {box-sizing: border-box;} html, body, section, div, input, button {border-radius: 0.2rem;font-family: Verdana, Geneva, sans-serif; font-size: 1.15rem; width:100%} body {border:0; padding:0%; margin:0; background-color:#FFF; color: #000;} input {padding:0.2rem 0.4rem; margin:0rem; background-color:#F0F0F0; color:#000;border: 2px solid #F0F0F0; outline-width: 0;} a {color:#0078D4} .button {border: 2px solid #0078D4;outline-width: 0; padding:0.2rem 0.4rem; margin:0.8rem 0rem;background-color:#0078D4;color:#FFF;cursor:pointer;} .bigscreen {padding:1rem 1.3rem 1rem 1rem; margin:0;} .screen {min-width:280px; max-width:420px; margin:auto;} .message {font-size:1.5rem; padding:0.2rem 0rem;} .label {padding:0.2rem 0.4rem 0rem 0.4rem; margin:0.2rem 0rem 0rem 0rem; font-size:0.8rem; background-color:#FFF; color:#000; border: 2px solid #FFF;}</style></head><body><div id=\"bigscreen\" class=\"bigscreen\"><div id=\"screen\" class=\"screen\"><div class=\"message\"><b>ESP32</b> "+ message +"</div>" +address+ "<p><form method='post' action='conf'><div class='label'>max_idle_secs</div><input name='max_idle_secs' value='"+pref_max_idle_secs+"' placeholder='max idle seconds'><div class='label'>time_to_sleep</div><input name='time_to_sleep' value='"+pref_time_to_sleep+"' placeholder='sleep-time seconds'><div class='label'>WiFi SSID</div><input name='ssid' value='"+pref_wifiSSID+"' placeholder='WiFi SSID'><div class='label'>WiFi password</div><input name='pass' type='password' placeholder='password'><input type='submit' value='save configuration' class=button></form><p><a href=\"/SLEEP\">DEEP SLEEP</a> ("+String(bootCount)+") | <a href=\"/RESTART\">RESTART</a><p>LED <a href=\"/H\">ON</a> | <a href=\"/L\">OFF</a> | DISPLAY <a href=\"/DISPLAYINIT\">INIT</a> | <a href=\"/SCANNETWORKS\">SCAN NETWORKS</a> | <a href=\"/TIME\">REQUEST TIME</a> | <a href=\"/MILLIS\">MILLIS</a>: "+String(millis())+"<br>SocketIO: "+String(sIOclient.sid)+"<br>COUNTER: "+String(counter)+"</div></div></body></html>";
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
	epaper_print_status2("SLEEPING " + String(pref_time_to_sleep) + " > touch PIN");
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


