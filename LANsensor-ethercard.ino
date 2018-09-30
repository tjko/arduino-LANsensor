/*
 * LANsensor - Arduino based LAN temperature/humidity sensor
 * Copyright (C) 2017-2018 Timo Kokkonen <tjko@iki.fi>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA. 	      
 *
 *
 * Updating sensor name (ENABLE_UPDATE_PAGE enabled):
 * http://x.x.x.x/update?name=sensorname
 * 
 */


#define ENABLE_SETUP_PAGE 0
#define ENABLE_UPDATE_PAGE 1

#include <Wire.h>
#include <HDC1050.h>
#include <WireScan.h>
#include <EEPROM.h>
#include <EtherCard.h>
#include <time.h>
#include <RTClib.h>
#include <TinyTZ.h>

RTC_DS3231 rtc;
DateTime now; // last read RTC time
struct tm tm; // last read RTC time in local time
char timestamp[29]; // space for storing isotime() string.

HDC1050 hdcSensor;

double temp_delta;
long bootcount;

#define SYSLOG_FACILITY   1  // USER
#define SYSLOG_SEVERITY   5  // NOTICE
#define SYSLOG_PORT 514

const char syslog_months[][4] PROGMEM = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };


#define DHCP_OPT_TIME_OFFSET   2
#define DHCP_OPT_LOG_SERVERS   7
#define DHCP_OPT_NTP_SERVERS   42
#define DHCP_OPT_PCODE         100

uint8_t custom_dhcp_options[] = { 
      DHCP_OPT_NTP_SERVERS,
      DHCP_OPT_PCODE,
      DHCP_OPT_LOG_SERVERS,
      DHCP_OPT_TIME_OFFSET,
      0 
    };
int32_t time_offset = 0;
uint8_t ntpip[4] = { 0, 0, 0, 0 };
uint8_t logip[4] = { 0, 0, 0, 0 };
uint8_t mac[6] = {0x42,0xbe,0xef,0x01,0x02,0x3};
bool ntp_enabled = false;
bool log_enabled = false;
byte log_int = 0;
byte Ethernet::buffer[600];
BufferFiller bfill;


#define DEFAULT_TIMEZONE  "UTC"

#define HTTP_HEADERS   "Server: Arduino LANsensor\r\n" \
                       "Connection: close\r\n" \
                       "Content-type: text/html; charset=iso-8859-1\r\n\r\n"

const char http_headers[] PROGMEM = HTTP_HEADERS;
const char http_ok[] PROGMEM = "HTTP/1.0 200 OK\r\n";

#define HTML_STYLE     "<style>" \
                       "form { margin: 0 auto; width: 500px; padding: 1em; border: 1px solid #ccc; border-radius: 1em; } " \
                       "label { display: inline-block; width: 150px; text-align: right; } " \
                       "input { width: 300px; box-sizing: border-box; border: 1px solid #999; } " \
                       "input:focus { border-color: #000; } " \
                       "</style>"

#if ENABLE_SETUP_PAGE
const char page_header[] PROGMEM = "HTTP/1.0 200 OK\r\n" HTTP_HEADERS
                                   "<html><head>" HTML_STYLE;
#endif              

#define NTPTOUNIX_OFFSET -2208988800UL



#define UNCONNECTED_ANALOG_PIN 0  // for reading "random" values
#define SAMPLE_RATE 5


// EEPROM ID
#define ROM_ID_0 0x42
#define ROM_ID_1 0x08

// EEPROM layout:
#define ROM_ID_IDX         0   // 2 bytes     EEPROM ID string
#define ROM_MAC_IDX        2   // 6 bytes     MAC address
#define ROM_NAME_IDX       8   // 32 bytes    sensor name
#define ROM_TZ_IDX         40  // 64 bytes    default timezone
#define ROM_IP_IDX         104 // 4 bytes     ip (static)
#define ROM_NETMASK_IDX    108 // 4 bytes     netmask (static)
#define ROM_GATEWAY_IDX    112 // 4 bytes     default gateway ip (static)
#define ROM_DNS_IDX        116 // 4 bytes     dns server ip (static)
#define ROM_NTP_IDX        120 // 4 bytes     ntp server ip (static)
#define ROM_DHCP_IDX       124 // 1 byte      1=DHCP, 0=static IP
#define ROM_LOGINT_IDX     125 // 1 byte      frequency for syslog updates in seconds (0 = disable)
#define ROM_LOG_IDX        126 // 4 bytes     syslog server ip (static)

#define ROM_TEMP_DELTA_IDX 252 // 4 bytes (double) 
#define ROM_BOOTCOUNT_IDX  256 // 4 bytes (long)

#define ROM_NAME_LEN  32
#define ROM_TZ_LEN 64

char sensor_name[ROM_NAME_LEN+1];
byte dhcp_enabled;

int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}


void syslog(byte facility, byte severity, const char *msg) {
  word pri = facility*8+severity;
  char buf[128];
  char mon[4];

  memcpy_P(mon,syslog_months[tm.tm_mon],4);
  snprintf_P(buf,sizeof(buf)-1,PSTR("<%d>%s %2d %02d:%02d:%02d %d.%d.%d.%d %s"),
           pri,mon,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,
           ether.myip[0],ether.myip[1],ether.myip[2],ether.myip[3],msg);
  buf[sizeof(buf)-1]=0;
  ether.sendUdp(buf, strlen(buf), SYSLOG_PORT, logip, SYSLOG_PORT);  
}


void readRTC() {
  now = rtc.now();                // read RTC clock
  uint32_t t = now.secondstime(); 
  localtime_r(&t, &tm);           // save localtime in tm
  isotime_r(&tm, timestamp);      // generate string in local time
  int l = strlen(timestamp);
  timestamp[l]=' ';
  timestamp[l+1]=0;
  strncat(timestamp, TinyTZ.timezone((tm.tm_isdst > 0 ? 1:0)), sizeof(timestamp)-strlen(timestamp));
}


// use analog reads from unconnected pin combnined with compile time to
// initialize random number generator...
const char build_time[] PROGMEM = __TIME__;

void initPRNG() {
  word a = analogRead(UNCONNECTED_ANALOG_PIN);
  delay(100);
  word b = analogRead(UNCONNECTED_ANALOG_PIN);
  long tmp1 = ((long)a << 16) | (long)b;
  long tmp2 = pgm_read_dword_near(build_time+(sizeof(build_time)-4));

  randomSeed(tmp1 ^ tmp2);
}


void dhcp_option_cb(uint8_t option, const byte *data, uint8_t len) {
  int res = 0;

  switch (option) {
    case DHCP_OPT_TIME_OFFSET:
      time_offset = 0;
      for (byte i = 0; i < 4; i++) 
        time_offset = (time_offset << 8) | data[i];
      Serial.print(F(" DHCP Time Offset: "));
      Serial.println(time_offset);
      break;

    case DHCP_OPT_LOG_SERVERS:
      ether.copyIp(logip,data);
      Serial.print(F(" DHCP Log Servers: "));
      for(byte i = 0; i < len; i+=4) {  
        if (i > 0) Serial.print(',');
        ether.printIp(&data[i]);
      }
      Serial.println();
      break;

    case DHCP_OPT_NTP_SERVERS:
      ether.copyIp(ntpip,data);
      Serial.print(F(" DHCP NTP Servers: "));
      for(byte i = 0; i < len; i+=4) {  
        if (i > 0) Serial.print(',');
        ether.printIp(&data[i]);
      }
      Serial.println();
      break;

    case DHCP_OPT_PCODE:
      Serial.print(F("    DHCP Timezone: "));
      char buf[64];
      memcpy(buf, data, (len < 64 ? len : 63));
      buf[(len < 64 ? len : 63)]=0;
      Serial.println(buf);
      TinyTZ.setTZ(buf);
      break;
      
    default:
      break;
  }
  
}


#if ENABLE_UPDATE_PAGE

int update_ip(char *s, word idx, byte *ip) {
  int l=strlen(s);
  if (l < 7 || (ether.parseIp(s, ip)!=0)) return 0;
  for (l=0;l < 4; l++) 
    EEPROM.write(idx + l, ip[l]);
  return 1;
}

void process_update(char *url) {
  const char sep[] = "&";
  char *s = strtok(url, sep);
  int l;
  byte ip[4];
  
  if (!s) return;

  while (s) {
    ether.urlDecode(s);

    if (strncmp_P(s, PSTR("name="), 5) == 0) {
      s+=5;
      l = strlen(s);
      if (l > ROM_NAME_LEN) l = ROM_NAME_LEN;
      if (l > 0) {
        for(int i=0; i < l; i++) EEPROM.write(ROM_NAME_IDX + i, s[i]);
        Serial.print(F("Name: "));
        Serial.println(s);
      }
      if (l < ROM_NAME_LEN) EEPROM.write(ROM_NAME_IDX + l, 0);      
    }
    else if (strncmp_P(s, PSTR("tz="), 3) == 0) {
      s+=3;
      l = strlen(s);
      if (l > ROM_TZ_LEN) l = ROM_TZ_LEN;
      if (l > 0) {
        for(int i=0; i < l; i++) EEPROM.write(ROM_TZ_IDX + i, s[i]);
        Serial.print(F("Timezone: "));
        Serial.println(s);
      }   
      if (l < ROM_TZ_LEN) EEPROM.write(ROM_TZ_IDX + l, 0);   
    }
    else if (strncmp_P(s, PSTR("dhcp="), 5) == 0) {
      s+=5;
      int v = atoi(s);
      EEPROM.write(ROM_DHCP_IDX, (v == 0 ? 0 : 1));
    }
    else if (strncmp_P(s, PSTR("logint="), 7) == 0) {
      s+=7;
      int v = atoi(s);
      if (v >= 0 && v <= 255) EEPROM.write(ROM_LOGINT_IDX, (byte)v);
    }
    else if (strncmp_P(s, PSTR("delta="), 6) == 0) {
      s+=6;
      double v = atoi(s)/1000.0;
      EEPROM.put(ROM_TEMP_DELTA_IDX, v);
    }
    else if (strncmp_P(s, PSTR("ip="), 3) == 0) {
      if (update_ip(s+3, ROM_IP_IDX, ip)) {
        Serial.print(F("IP: "));
        ether.printIp(ip);
        Serial.println();  
      }     
    }
    else if (strncmp_P(s, PSTR("nm="), 3) == 0) {
      if (update_ip(s+3, ROM_NETMASK_IDX, ip)) {
        Serial.print(F("Netmask: "));
        ether.printIp(ip);
        Serial.println();  
      }     
    }
    else if (strncmp_P(s, PSTR("gw="), 3) == 0) {
      if (update_ip(s+3, ROM_GATEWAY_IDX, ip)) {
        Serial.print(F("Gateway: "));
        ether.printIp(ip);
        Serial.println();  
      }     
    }
    else if (strncmp_P(s, PSTR("dns="), 4) == 0) {
      if (update_ip(s+4, ROM_DNS_IDX, ip)) {
        Serial.print(F("DNS: "));
        ether.printIp(ip);
        Serial.println();  
      }     
    }
    else if (strncmp_P(s, PSTR("ntp="), 4) == 0) {
      if (update_ip(s+4, ROM_NTP_IDX, ip)) {
        Serial.print(F("NTP: "));
        ether.printIp(ip);
        Serial.println();  
      }     
    }


    s = strtok(NULL, sep);
  }
}
#endif

void setup() {
  int res,i = 0;
  
  Serial.begin(115200);
  while (!Serial && i < 1000) { 
    delay(1); 
    i++;
  }
  Serial.println(F("Arduino LANsensor by tjko@iki.fi (" __DATE__ ")"));
  Serial.print(F("SRAM Free: "));
  Serial.println(freeRam());


  // Check if EEPROM needs to be initialized...

  if (!(EEPROM.read(ROM_ID_IDX) == ROM_ID_0 && EEPROM.read(ROM_ID_IDX+1) == ROM_ID_1)) {  
    char tmp[64];

    Serial.println(F("Initalizing EEPROM..."));
    
    // randomize MAC...
    initPRNG();
    for (i = 3; i < 6; i++) mac[i] = (byte)random(0xff);        

    // clear entire EEPROM...
    for (i = 0; i < EEPROM.length(); i++) EEPROM.write(i, 0);

    // EEPROM id
    EEPROM.write(ROM_ID_IDX + 0, ROM_ID_0);
    EEPROM.write(ROM_ID_IDX + 1, ROM_ID_1);
    // MAC address
    for (i = 0; i < 6; i++) EEPROM.write(ROM_MAC_IDX + i, mac[i]);
    // Sensor Name
    memset(tmp, 0, sizeof(tmp));
    strcpy_P(tmp, PSTR("No Name"));
    for (i = 0; i < ROM_NAME_LEN; i++) EEPROM.write(ROM_NAME_IDX + i, tmp[i]);
    // Timezone
    memset(tmp, 0, sizeof(tmp));
    strcpy_P(tmp, PSTR(DEFAULT_TIMEZONE));
    for (i = 0; i < ROM_TZ_LEN; i++) EEPROM.write(ROM_TZ_IDX + i, tmp[i]);
    // IP
    EEPROM.write(ROM_IP_IDX + 0, 192);
    EEPROM.write(ROM_IP_IDX + 1, 168);
    EEPROM.write(ROM_IP_IDX + 2, 1);
    EEPROM.write(ROM_IP_IDX + 3, 42);
    // Netmask
    EEPROM.write(ROM_NETMASK_IDX + 0, 255);
    EEPROM.write(ROM_NETMASK_IDX + 1, 255);
    EEPROM.write(ROM_NETMASK_IDX + 2, 255);
    EEPROM.write(ROM_NETMASK_IDX + 3, 0);
    // Gateway    
    EEPROM.write(ROM_GATEWAY_IDX + 0, 192);
    EEPROM.write(ROM_GATEWAY_IDX + 1, 168);
    EEPROM.write(ROM_GATEWAY_IDX + 2, 1);
    EEPROM.write(ROM_GATEWAY_IDX + 3, 1);
    // DNS Server
    //EEPROM.write(ROM_DNS_IDX + 0, 0);
    //EEPROM.write(ROM_DNS_IDX + 1, 0);
    //EEPROM.write(ROM_DNS_IDX + 2, 0);
    //EEPROM.write(ROM_DNS_IDX + 3, 0);
    // NTP Server
    //EEPROM.write(ROM_NTP_IDX + 0, 0);
    //EEPROM.write(ROM_NTP_IDX + 1, 0);
    //EEPROM.write(ROM_NTP_IDX + 2, 0);
    //EEPROM.write(ROM_NTP_IDX + 3, 0);
    // DHCP flag
    EEPROM.write(ROM_DHCP_IDX, 1);       // 1=DHCP enabled, 0=static IP
    // Syslog interval (seconds)
    EEPROM.write(ROM_LOGINT_IDX, 5);     // 5 minutes
    // Syslog Server
    //EEPROM.write(ROM_LOG_IDX + 0, 0);
    //EEPROM.write(ROM_LOG_IDX + 1, 0);
    //EEPROM.write(ROM_LOG_IDX + 2, 0);
    //EEPROM.write(ROM_LOG_IDX + 3, 0);
    

    // Temperature offset/delta...
    temp_delta = 0.0;
    EEPROM.put(ROM_TEMP_DELTA_IDX, temp_delta);

    // Boot counter
    bootcount=0;
    EEPROM.put(ROM_BOOTCOUNT_IDX, bootcount);
  }


  // Read configuration from EEPROM...
  
  if (EEPROM.read(ROM_ID_IDX) == ROM_ID_0 && EEPROM.read(ROM_ID_IDX+1) == ROM_ID_1 ) {
    
    // read MAC address...
    for(i = 0; i < 6; i++) mac[i] = EEPROM.read(ROM_MAC_IDX + i);
    
    // DHCP flag
    dhcp_enabled = EEPROM.read(ROM_DHCP_IDX);
    
    // NTP ip
    for (i=0; i<4; i++) ntpip[i] = EEPROM.read(ROM_NTP_IDX + i);

    // LOG ip
    for (i=0; i<4; i++) ntpip[i] = EEPROM.read(ROM_LOG_IDX + i);

    // LOG interval
    log_int = EEPROM.read(ROM_LOGINT_IDX);

    // Sensor name
    for (i=0; i<ROM_NAME_LEN; i++) sensor_name[i] = EEPROM.read(ROM_NAME_IDX + i);
    sensor_name[ROM_NAME_LEN] = 0;      
    Serial.print(F("Sensor name: "));
    Serial.println(sensor_name);

    // Timezone
    char tz[ROM_TZ_LEN+1];
    for (i=0; i<ROM_TZ_LEN; i++) tz[i] = EEPROM.read(ROM_TZ_IDX + i);
    tz[ROM_TZ_LEN] = 0;
    TinyTZ.setTZ(tz);  
    Serial.print(F("Default timezone: "));
    Serial.println(tz);

    // read temperature delta (adjustment)
    EEPROM.get(ROM_TEMP_DELTA_IDX, temp_delta);
    Serial.print(F("Temperature delta: "));
    Serial.println(temp_delta);
    
    // read/update boot count...
    EEPROM.get(ROM_BOOTCOUNT_IDX, bootcount);
    bootcount++;
    EEPROM.put(ROM_BOOTCOUNT_IDX, bootcount);
    Serial.print(F("Boot counter: "));
    Serial.println(bootcount);
  } else {
    Serial.println(F("EEPROM error!"));
    while (1);
  }



  // Initialize and scan I2C Bus...
  
  Wire.begin();
  WireScan.scan(&Serial);


  // Initialize temperature senson...

  Serial.println(F("Initializing Sensor..."));
  hdcSensor.reset();
  Serial.println(F("  Model: HDC1080"));
  Serial.print(F("  Manufacturer & Device ID: "));
  Serial.print(hdcSensor.getManufacturerID(), HEX);
  Serial.print(F(":"));
  Serial.println(hdcSensor.getDeviceID(), HEX);
  Serial.print(F("  Serial Number: "));
  Serial.println(hdcSensor.getSerialID());


  // Initialize RTC clock... 

  Serial.println(F("Initializing RTC..."));
  while (! rtc.begin()) {
    Serial.println(F("Cannot find RTC"));
    delay(1000);
  }
  if (rtc.lostPower()) {
    Serial.println(F("RTC has lost power. Resetting..."));
    rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));
  }
  readRTC();
  Serial.print(F("  Time: "));
  Serial.println(timestamp);


  // Initialize Ethernet interface...
  
  Serial.println(F("Initializing NIC..."));
  Serial.print(F("      MAC Address: "));
  for(int i = 0; i < 6; i++) { 
      char c;
      if (i > 0) { c=':'; Serial.print(c); }
      if (mac[i] < 16) { c='0'; Serial.print(c); }
      Serial.print(mac[i],HEX);
  }
  Serial.println();
  while (!(res = ether.begin(sizeof Ethernet::buffer,mac))) {
    Serial.println(F("Failed to initialize Ethernet controller"));
  }
  Serial.print(F("     ENC28J60 rev: "));
  Serial.println(res);
  if (dhcp_enabled) {
    Serial.println(F("Sending DHCP request..."));
    ether.dhcpAddOptionCallback(custom_dhcp_options, dhcp_option_cb);
    i=0;
    while (dhcp_enabled && !ether.dhcpSetup()) {
      Serial.println(F("DHCP error"));
      if (++i >= 3) dhcp_enabled = 0;
    }
  }
  if (!dhcp_enabled) {
    // setup NIC with static IP from EEPROM...
    Serial.println(F("Configuring static IP..."));
    byte ip[4],netmask[4],gateway[4],dns[4];
    for (i=0; i<4; i++) ip[i] = EEPROM.read(ROM_IP_IDX + i);
    for (i=0; i<4; i++) netmask[i] = EEPROM.read(ROM_NETMASK_IDX + i);
    for (i=0; i<4; i++) gateway[i] = EEPROM.read(ROM_GATEWAY_IDX + i);
    for (i=0; i<4; i++) dns[i] = EEPROM.read(ROM_DNS_IDX + i);
    ether.staticSetup(ip,gateway,dns,netmask);
  }
  ether.printIp(F("  IP Address: "), ether.myip);
  ether.printIp(F("     Netmask: "), ether.netmask);
  ether.printIp(F("     Gateway: "), ether.gwip);
  ether.printIp(F("  DNS Server: "), ether.dnsip);
  if (dhcp_enabled) 
    ether.printIp(F(" DHCP Server: "), ether.dhcpip);
  ether.printIp(F("  NTP Server: "), ntpip);
  if (!(ntpip[0]==0 && ntpip[1]==0 && ntpip[2]==0 & ntpip[3]==0))
    ntp_enabled=true;
  Serial.print(F("  Log Server: "));
  ether.printIp(logip);
  Serial.print(' ');
  Serial.print(log_int);
  Serial.println(F("min"));
  if (!(logip[0]==0 && logip[1]==0 && logip[2]==0 & logip[3]==0))
    log_enabled=true;
  

  readRTC();
  Serial.print(F("   Timezone: "));
  Serial.print(TinyTZ.timezone((tm.tm_isdst > 0 ? 1:0)));
  Serial.print(' ');
  Serial.println(TinyTZ.offset((tm.tm_isdst > 0 ? 1:0))/3600);

  Serial.println(F("==="));
}


// =======================================================================
// main program loop

long counter = 0;
float tc, tf, h;
unsigned long samplecount = 0;
unsigned long sample_t = 0;
unsigned long ntpquery_t = 0;
unsigned long syslog_t = 0;
unsigned long ticks;
float tc_min, tc_max, h_min, h_max, tc_range, h_range;
char tc_s[8],tf_s[8],h_s[8];
int ntpclientport = 123;

void loop() {
  ticks = millis();
  

  // process network packets...
  uint16_t plen = ether.packetReceive();
  uint16_t pos = ether.packetLoop(plen);
  if (pos > 0) {
    // new TCP connection...
    char *tcpdata = (char*) Ethernet::buffer + pos;
    bfill = ether.tcpOffset();

    readRTC();
    //Serial.print(timestamp);
    //Serial.print(F(": New TCP connection: len="));
    //Serial.println(plen);

#if 0
    Serial.print('|');
    char query[200];
    memcpy(query, tcpdata,sizeof(query)-1);
    query[sizeof(query)-1]=0;
    Serial.print(query);
    Serial.println('|');
#endif
    

    if ( (strncmp_P(tcpdata, PSTR("GET / "), 6) == 0) ||
         (strncmp_P(tcpdata, PSTR("GET /index"), 10) == 0) ) {
        uint32_t secs = ticks/1000;
        uint16_t h = secs/3600;
        uint16_t m = (secs/60)%60;
        uint16_t s = secs%60;
        bfill.emit_p(PSTR("$F$F" 
                         "<html><head><title>LANsensor</title>" 
                         "<style>#b1 { background: #016567; border-radius: 10px; padding: 10px; } "
                         " #l1 { background: #03a3aa; border-radius: 5px; padding: 10px; } " 
                         "</style></head>\n"
                         "<body><table id=\"b1\"><tr><td><h2><font color=#03a3aa>Arduino LANsensor: $S</font></h2><p>"
                         "<tr><td id=\"l1\">Temperature: $S C ($S F)<br>"
                         "Humidity: $S %<br>"
                         "<tr><td><font size=-1 color=#03a3aa>Time: $S<br>"
                         "Uptime: $D$D:$D$D:$D$D  Boot count: $L<br></font>"
                         "</body></html>\n"),
                         http_ok,http_headers,sensor_name,tc_s,tf_s,h_s,timestamp,
                         h/10,h%10,m/10,m%10,s/10,s%10,bootcount);
      //Serial.print(F("home len="));
      //Serial.println(bfill.position());
      ether.httpServerReply(bfill.position());
    } else if (strncmp_P(tcpdata, PSTR("GET /raw"), 7) == 0) {
        bfill.emit_p(PSTR("$F$F" 
                         "LANsensor,$S,C,$S,%,$S,$S,$L,$L\n"),
                         http_ok,http_headers,
                         tc_s,h_s,sensor_name,timestamp,ticks,bootcount);
      //Serial.print("raw len=");           
      //Serial.println(bfill.position());
      ether.httpServerReply(bfill.position());
#if ENABLE_SETUP_PAGE     
    } else if (strncmp_P(tcpdata, PSTR("GET /setup"), 7) == 0) {
        ether.httpServerReplyAck();
        memcpy_P(ether.tcpOffset(), page_header, sizeof(page_header));
        Serial.println(sizeof(page_header));
        ether.httpServerReply_with_flags(sizeof(page_header)-1,TCP_FLAGS_ACK_V);
        bfill.emit_p(PSTR(// "HTTP/1.0 200 OK\n$F" 
                          //"<html><head><title>LANsensor</title>" "</head>\n"
                         "<title>LANsensor</title></head>\n"
                         "<body><h1>Arduino LANsensor: $S</h1><h2>Setup</h2><p>"
                         "<form action=\"/update\" method=\"get\">"
                         " <div><label for=\"name\">Sensor Name:</label><input type=\"text\" id=\"name\" name=\"sname\" maxlength=32 value=\"$S\"></div>"
                         " <div><label for=\"tz\">Timezone:</label><input type=\"text\" id=\"tz\" name=\"stz\" maxlength=64></div>"
                         " <div class=\"button\"><button type=\"submit\">Update</button></div>"
                         "</form>"
                         "<div><hr><font size=-1>Time: $S<br></body></html>\n"),
                         sensor_name,sensor_name,timestamp);           
      Serial.print(F("setup len="));
      Serial.println(bfill.position());
      ether.httpServerReply_with_flags(bfill.position(),TCP_FLAGS_ACK_V|TCP_FLAGS_FIN_V);
#endif
#if ENABLE_UPDATE_PAGE
    } else if (strncmp_P(tcpdata, PSTR("GET /update?"), 12) == 0) {
      char *s = tcpdata + 12;
      char *e = tcpdata + plen;
      while (s < e && *s != ' ') s++;
      if (s < e) {
        *s=0;
        process_update(tcpdata + 12);
        bfill.emit_p(PSTR("$F$F"
                          "EEPROM updated. Please reset device\n"),
                          http_ok,http_headers,timestamp);           
        //Serial.print(F("update len="));
        //Serial.println(bfill.position());
        ether.httpServerReply(bfill.position());
      }
#endif
    } else {
      bfill.emit_p(PSTR("HTTP/1.0 403 Forbidden\n$F"
                       "<html><head><title>403 Forbidden</title></head>\n"
                       "<body><h1>403 Forbidden</h1></body></html>\n"), http_headers);
      //Serial.print(F("403 len="));
      //Serial.println(bfill.position());
      ether.httpServerReply(bfill.position());
    }
  }
  else if (plen > 0) {
    // unprocessed (UDP) packet...
    uint32_t ntime = 0;
    if (ether.ntpProcessAnswer(&ntime,ntpclientport)) {
      readRTC();
      ntime += NTPTOUNIX_OFFSET;
      Serial.print(timestamp);
      Serial.print(F(": NTP response len="));
      Serial.print(plen);
      Serial.print(F(" time="));
      Serial.print(ntime);
      Serial.print(F("  RTC: time="));
      Serial.print(now.unixtime());
      if (ntime != now.unixtime()) {
        Serial.print(F(" Adjusting RTC clock..."));
        rtc.adjust(DateTime(ntime));
        Serial.print(F("Done."));
      }  
      Serial.println();
      ntpquery_t += 86400 * 1000;
    }
  }


  // check if should send a NTP query...

  if (ntp_enabled && (ticks > ntpquery_t + (5 * 1000))) {
    readRTC();
    Serial.print(timestamp);
    Serial.print(F(": Sending NTP query to "));
    ether.printIp(ntpip);
    Serial.println();
    ntpquery_t = ticks;
    ether.ntpRequest(ntpip,ntpclientport);
  }


  // send syslog packets on set intervals...

  if (log_enabled && (ticks > syslog_t + ((long)log_int * 60 * 1000) || ticks < syslog_t)) {
    char buf[80];
    readRTC();
    Serial.print(timestamp);
    Serial.print(F(": Sending Syslog packet to "));
    ether.printIp(logip);
    Serial.println();
    snprintf_P(buf,sizeof(buf)-1,PSTR("LANsensor: %s,C,%s,%%,%s,%s,%lu,%lu"),
             tc_s,h_s,sensor_name,timestamp,ticks,bootcount);           
    buf[sizeof(buf)-1]=0;
    syslog(SYSLOG_FACILITY, SYSLOG_SEVERITY, buf);
    syslog_t = ticks;
  }

  // read sensor once every SAMPLE_RATE seconds or so... 
   
  if (ticks > sample_t + (SAMPLE_RATE*1000) || ticks < sample_t) {
    readRTC();
    hdcSensor.getTemperatureHumidity(tc, h);
    tc += temp_delta;
    tf = tc*1.8+32;
    if (samplecount < 1) {
      tc_min=tc_max=tc;
      h_min=h_max=h;
    } else {
      if (tc < tc_min) { tc_min=tc; }
      if (tc > tc_max) { tc_max=tc; }
      if (h < h_min) { h_min=h; }
      if (h > h_max) { h_max=h; }
    }
    samplecount++;
    sample_t = ticks;
    tc_range=tc_max - tc_min;
    h_range=h_max - h_min;

    dtostrf(tc,-1,2,tc_s);
    dtostrf(tf,-1,2,tf_s);
    dtostrf(h,-1,1,h_s);
    Serial.print(timestamp);
#if 0
    Serial.print(',');
    Serial.print(ticks);
    Serial.print(',');
    Serial.print(samplecount);
#endif
    Serial.print(F(": "));
    Serial.print(tc_s);
    Serial.print(F("C [min="));
    Serial.print(tc_min);
    Serial.print(F(", max="));
    Serial.print(tc_max);
    Serial.print(F(", range="));
    Serial.print(tc_range);
    Serial.print(F("]  "));
    Serial.print(tf_s);
    Serial.print(F("F  "));
    Serial.print(h_s);
    Serial.print(F("%H [min="));
    Serial.print(h_min);
    Serial.print(F(", max="));
    Serial.print(h_max);
    Serial.print(F(", range="));
    Serial.println(h_range);
    //Serial.print(F("]  SRAM free:"));
    //Serial.println(freeRam());   
  }

    //delayMicroseconds(10);
}

