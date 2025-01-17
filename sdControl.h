#ifndef _SD_CONTROL_H_
#define _SD_CONTROL_H_

#define SPI_BLOCKOUT_BOOTUP_PERIOD	10UL // Second
#define SPI_BLOCKOUT_PERIOD	5UL // microsecond
#define SPI_BLOCKOUT_PERIOD_MS (SPI_BLOCKOUT_PERIOD * 1000UL)

class SDControl {
public:
  SDControl() { }
  static void setup();
  static void takeControl();
  static void relinquishControl();
  static int canWeTakeControl();
  static bool wehaveControl();
  static bool printerRequest();
  void deleteFile(String path);

private:
  static volatile long _spiBlockoutTime;
  static bool _weTookBus;
  static volatile bool _printerRequest;
};

extern SDControl sdcontrol;

#endif
