#include "config.h"

Config config;
#include <SD.h>
#include "sdControl.h"


int Config::loadFS() {
    SERIAL_ECHOLN("Going to load config from SD card");

    File file = SD.open(CONFIG_FILE, FILE_READ);
    if (!file) {
        SERIAL_ECHOLN("Failed to open config file from SD");
        return -1;
    }

    SERIAL_ECHOLN("File Size: " + String(file.size()));
    
    SERIAL_ECHOLN("File contents:");
    String entireFile = "";
    while (file.available()) {
        entireFile += (char)file.read();
    }
    SERIAL_ECHOLN(entireFile);
    
    file.seek(0);

    int rst = 0, step = 0;
    String buffer, sKEY, sValue;
    while (file.available()) {
        buffer = file.readStringUntil('\n');
        SERIAL_ECHOLN("Raw Buffer: '" + buffer + "'");
        
        if (buffer.length() == 0) {
            SERIAL_ECHOLN("Empty line - skipping");
            continue;
        }
        
        buffer.trim();
        buffer.replace("\r", "");
        
        SERIAL_ECHOLN("Processed Buffer: '" + buffer + "'");
        
        int iS = buffer.indexOf('=');
        if (iS < 0) {
            SERIAL_ECHOLN("No '=' found - skipping line");
            continue;
        }
        
        sKEY = buffer.substring(0, iS);
        sValue = buffer.substring(iS + 1);
        
        sKEY.trim();
        sValue.trim();
        
        SERIAL_ECHOLN("Parsed - Key: '" + sKEY + "', Value: '" + sValue + "'");

        if (sKEY == "SSID") {
            SERIAL_ECHOLN("INI file : SSID found with length: " + String(sValue.length()));
            if (sValue.length() > 0) {
                memset(data.ssid, '\0', WIFI_SSID_LEN);
                sValue.toCharArray(data.ssid, WIFI_SSID_LEN);
                SERIAL_ECHOLN("SSID set to: " + String(data.ssid));
                step++;
            } else {
                SERIAL_ECHOLN("SSID value is empty!");
                rst = -2;
                goto FAIL;
            }
        } else if (sKEY == "PASSWORD") {
            SERIAL_ECHOLN("INI file : PASSWORD found with length: " + String(sValue.length()));
            if (sValue.length() > 0) {
                memset(data.psw, '\0', WIFI_PASSWD_LEN);
                sValue.toCharArray(data.psw, WIFI_PASSWD_LEN);
                SERIAL_ECHOLN("Password set to: " + String(data.psw));
                step++;
            } else {
                SERIAL_ECHOLN("Password value is empty!");
                rst = -3;
                goto FAIL;
            }
        } else {
            SERIAL_ECHOLN("Unknown key: " + sKEY);
            continue;
        }
    }

    if (step != 2) {
        SERIAL_ECHOLN("Missing configuration - steps completed: " + String(step));
        rst = -4;
        goto FAIL;
    }

FAIL:
    file.close();
    return rst;
}

bool Config::validateEEPROMData() {
    SERIAL_ECHO("Validating EEPROM - Flag: ");
    SERIAL_ECHOLN(data.flag);
    
    if (!data.flag) {
        SERIAL_ECHOLN("Flag check failed");
        return false;
    }
    
    size_t ssidLen = strnlen(data.ssid, WIFI_SSID_LEN);
    SERIAL_ECHO("SSID Length: ");
    SERIAL_ECHO(ssidLen);
    SERIAL_ECHO(" SSID: ");
    SERIAL_ECHOLN(data.ssid);
    
    if (ssidLen == 0 || ssidLen >= WIFI_SSID_LEN) {
        SERIAL_ECHOLN("SSID length check failed");
        return false;
    }
    
    size_t pswLen = strnlen(data.psw, WIFI_PASSWD_LEN);
    SERIAL_ECHO("PSW Length: ");
    SERIAL_ECHOLN(pswLen);
    
    if (pswLen == 0 || pswLen >= WIFI_PASSWD_LEN) {
        SERIAL_ECHOLN("Password length check failed");
        return false;
    }
    
    for (size_t i = 0; i < ssidLen; i++) {
        if (data.ssid[i] < 32 || data.ssid[i] > 126) return false;
    }
    
    for (size_t i = 0; i < pswLen; i++) {
        if (data.psw[i] < 32 || data.psw[i] > 126) return false;
    }
    
    return true;
}

unsigned char Config::load(FS* fs) {
    // Try EEPROM first
    SERIAL_ECHOLN("Attempting to load from EEPROM");
    EEPROM.begin(EEPROM_SIZE);
    uint8_t *p = (uint8_t*)(&data);
    for (int i = 0; i < sizeof(data); i++) {
        *(p + i) = EEPROM.read(i);
    }
    EEPROM.commit();

    if (validateEEPROMData()) {
        SERIAL_ECHOLN("Successfully loaded valid config from EEPROM");
        return data.flag;
    }

    // If EEPROM fails, try SD card
    if (sdcontrol.canWeTakeControl() != -1) {
        SERIAL_ECHOLN("SD card access granted, attempting to load config");
        sdcontrol.takeControl();
        
        if (0 == loadFS()) {
            SERIAL_ECHOLN("Successfully loaded from SD, saving to EEPROM");
            save(data.ssid, data.psw);
            sdcontrol.relinquishControl();
            return 1;
        }
        sdcontrol.relinquishControl();
        SERIAL_ECHOLN("Failed to load from SD card");
    }

    // Fall back to defaults if both methods fail
    SERIAL_ECHOLN("No valid config found, using defaults");
    const char* default_ssid = "ssid";
    const char* default_password = "password";
    save(default_ssid, default_password);
    return 0;
}

char* Config::ssid() {
  return data.ssid;
}

void Config::ssid(char* ssid) {
  if (ssid == NULL) return;
  strncpy(data.ssid, ssid, WIFI_SSID_LEN);
}

char* Config::password() {
  return data.psw;
}

void Config::password(char* password) {
  if (password == NULL) return;
  strncpy(data.psw, password, WIFI_PASSWD_LEN);
}

void Config::save(const char* ssid, const char* password) {
  if (ssid == NULL || password == NULL)
    return;

  EEPROM.begin(EEPROM_SIZE);
  data.flag = 1;
  strncpy(data.ssid, ssid, WIFI_SSID_LEN);
  strncpy(data.psw, password, WIFI_PASSWD_LEN);
  uint8_t *p = (uint8_t*)(&data);
  for (int i = 0; i < sizeof(data); i++) {
    EEPROM.write(i, *(p + i));
  }
  EEPROM.commit();
}

void Config::save() {
  if (data.ssid == NULL || data.psw == NULL)
    return;
  EEPROM.begin(EEPROM_SIZE);
  data.flag = 1;
  uint8_t *p = (uint8_t*)(&data);
  for (int i = 0; i < sizeof(data); i++) {
    EEPROM.write(i, *(p + i));
  }
  EEPROM.commit();
}

void Config::clear() {
  EEPROM.begin(EEPROM_SIZE);
  data.flag = 0;
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}