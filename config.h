#ifndef CONFIG_H
#define CONFIG_H

#include <FS.h>
#include <EEPROM.h>
#include "serial.h"

#define CONFIG_FILE "/config.txt"
#define WIFI_SSID_LEN 32
#define WIFI_PASSWD_LEN 64
#define EEPROM_SIZE 512

struct ConfigData {
  char ssid[WIFI_SSID_LEN];
  char psw[WIFI_PASSWD_LEN];
  uint8_t flag;
};

class Config {
public:
  int loadFS();
  unsigned char load(FS* fs);
  char* ssid();
  void ssid(char* ssid);
  char* password();
  void password(char* password);
  void save(const char* ssid, const char* password);
  void save();
  void clear();

private:
  FS* _fs;
  ConfigData data;
};

extern Config config;

#endif // CONFIG_H