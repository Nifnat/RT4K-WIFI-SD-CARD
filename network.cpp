#include "network.h"
#include "serial.h"
#include "config.h"
#include "pins.h"
#include "sdControl.h"
#include <SPIFFS.h>

#if defined(ARDUINO) && ARDUINO >= 100
    #include "Arduino.h"
#else
    #include "WProgram.h"
#endif

#ifdef ESP32
  #include <ESPmDNS.h>
#elif defined(ESP8266)
  #include <ESP8266mDNS.h>
#endif

#ifdef ESP32
  #include <WiFi.h>
  #include <esp_pm.h>
  #include <esp_wifi.h>
  #include <esp_wifi_types.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif


IPAddress AP_local_ip(192, 168, 4, 1);
IPAddress AP_gateway(192, 168, 4, 1);
IPAddress AP_subnet(255, 255, 255, 0);  // Subnet
const char* AP_SSID  = "RT4K-SD-WIFI";

String IpAddress2String(const IPAddress& ipAddress)
{
  return String(ipAddress[0]) + String(".") +\
  String(ipAddress[1]) + String(".") +\
  String(ipAddress[2]) + String(".") +\
  String(ipAddress[3]);
}

int Network::startConnect(String ssid, String psd) {
  if(WiFi.status() == WL_CONNECTED && WiFi.SSID().equals(_ssid.c_str())) {
    return 0; // already connected to the AP
  }

  _ssid = ssid;
  _psd = psd;
  _doConnect = true;

  return 1;
}

int Network::connect(String ssid, String psd) {
  if(_wifiMode == _stamode && WiFi.status() == WL_CONNECTED && WiFi.SSID().equals(ssid.c_str())) {
    SERIAL_ECHOLN("Aready connected to the AP");
    return 1;
  }

  // startSoftAP();

  wifiConnected = false;
  wifiConnecting = true;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_local_ip, AP_gateway, AP_subnet);
  SERIAL_ECHO("Connecting to ");SERIAL_ECHOLN(ssid.c_str());
  WiFi.begin(ssid.c_str(), psd.c_str());

  // Wait for connection
  unsigned int timeout = 0;
  while(WiFi.status() != WL_CONNECTED) {
    //blink();
    Serial.print(".");
    timeout++;
    if(timeout++ > WIFI_CONNECT_TIMEOUT/100) {
      Serial.println("");
      wifiConnecting = false;
      Serial.println("Connect fail, please check your SSID and password");
      return 2;
    }
    else
      delay(500);
  }

  config.save(ssid.c_str(), psd.c_str());

  Serial.println("");
  Serial.print("Connected to "); Serial.println(config.ssid());
  Serial.print("IP address: "); Serial.println(WiFi.localIP());

  // Initialize mDNS
  // if (!MDNS.begin(HOSTNAME)) {
  //     Serial.println("Error setting up MDNS responder!");
  // } else {
  //     Serial.print("mDNS responder started - hostname: ");
  //     Serial.print(HOSTNAME);
  //     Serial.println(".local");
  //     // Add service to mDNS
  //     MDNS.addService("http", "tcp", 80);
  // }

  _stamode = true;
  wifiConnected = true;
  wifiConnecting = false;

  return 3;
}

int Network::start() {
  if(config.load(&SPIFFS) != 1) { // Not connected before
    // Start the AP
    startSoftAP();
    return 1;
  }
  wifiConnected = false;
  wifiConnecting = true;
  
  WiFi.mode(WIFI_STA);
  SERIAL_ECHO("Connecting to ");SERIAL_ECHOLN(config.ssid());
  WiFi.begin(config.ssid(), config.password());

  // Wait for connection
  unsigned int timeout = 0;
  while(WiFi.status() != WL_CONNECTED) {
    //blink();
    SERIAL_ECHO(".");
    timeout++;
    if(timeout++ > WIFI_CONNECT_TIMEOUT/500) {
      SERIAL_ECHOLN("");
      wifiConnecting = false;
      SERIAL_ECHOLN("Connect fail, please check your INI file");
      config.clear();
      startSoftAP();
      SERIAL_ECHOLN("Change to AP mode\n. AP RT4K-SD-WIFI started");
      return 2;
    }
    else
      delay(500);
  }

  config.save();

  SERIAL_ECHOLN("");
  SERIAL_ECHO("Connected to "); SERIAL_ECHOLN(config.ssid());
  SERIAL_ECHO("IP address: "); SERIAL_ECHOLN(WiFi.localIP());

  // Set power mode to min modem
  if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) {
      SERIAL_ECHOLN("Failed to enable minimum modem power save");
  }

  wifiConnected = true;
  wifiConnecting = false;
  _stamode = true;

  // // Initialize mDNS
  // if (!MDNS.begin(HOSTNAME)) {
  //     Serial.println("Error setting up MDNS responder!");
  // } else {
  //     Serial.print("mDNS responder started - hostname: ");
  //     Serial.print(HOSTNAME);
  //     Serial.println(".local");
  //     // Add service to mDNS
  //     MDNS.addService("http", "tcp", 80);
  // }

  return 3;
}

int Network::status() {
  if(wifiConnected) {
    return 3; // connected
  }
  else {
    if(wifiConnecting) return 2; // connecting
    else return 1; // fail
  }
}

bool Network::isConnected() {
  return wifiConnected;
}

bool Network::isConnecting() {
  return wifiConnecting;
}

bool Network::isSTAmode() {
  return _stamode;
}

void Network::startSoftAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_local_ip, AP_gateway, AP_subnet);
  if (WiFi.softAP(AP_SSID)) {                           
    _stamode = false;
    Serial.println("RT4K-SD-WIFI SoftAP started.");
    Serial.print("IP address = ");
    Serial.println(WiFi.softAPIP());
    Serial.println(String("MAC address = ")  + WiFi.softAPmacAddress().c_str());
    config.clear();
  } else { 
    Serial.println("WiFiAP Failed");
    delay(1000);
    Serial.println("restart now...");
    ESP.restart();
  }
}

void Network::getWiFiList(String &list) {
  list = _wifiList;
}

void Network::doScan() {
  _doScan = true;
}

void Network::scanWiFi() {
  Serial.println("Scanning wifi");
  Serial.println("--------->");
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  _wifiList = "[";
  if (n != 0) {
    Serial.print(n);
    Serial.println(" networks found");
    for (int i = 0; i < n; ++i) {
      _wifiList += "{\"ssid\":\"";
      _wifiList += WiFi.SSID(i);
      _wifiList += "\",\"rssi\":\"";
      _wifiList += WiFi.RSSI(i);
      _wifiList += "\"";
      _wifiList += ",\"type\":\"";
      _wifiList += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)? "open":"close";
      _wifiList += "\"";
      _wifiList += "}";
      if(i!=n-1) _wifiList += ",";
    }
  }
  _wifiList += "]";
  Serial.println(_wifiList);
}

void Network::loop() {

  #ifdef ESP8266
        MDNS.update();
  #endif
  
  if(_doConnect) {
    connect(_ssid,_psd);
    _doConnect = false;
  }

  if(_doScan) {
    scanWiFi();
    _doScan = false;
  }
}

class Network network;