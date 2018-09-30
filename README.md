# arduino-LANsensor
Arduino based Networked Temperature and Humidity Sensor 

![LANSensor UI](img/LANsensor-ui.jpg?raw=true "LANsensor UI")

High precision temperature and humidity sensor with network (LAN) connections. With simple Web interface
and support for logging data using syslog.

This was created as an experiment to see how well Arduino can control cheap ENC28J60 LAN modules. 
Turns out surprisingly well, altough 32KB flash size is clear limitation (no space implement any fancy UI,
due to shee size of HTML code needed...)


## Components
- Adruino Nano (or compatible)
- HDC1080 sensor module (for example "CJMCU-1080")
- ENC28J60 Ethernet LAN Module for Arduino
- DS3231 IIC High Precision Real Time Clock Module [optional]
- 3.3V Voltage reulator module (to power ENC28J60)

## Enclosure

3D Printed encolsure example can be found in Thingiverse: [https://www.thingiverse.com/thing:3127838]

Images:
![Finished Unit](img/LANsensor-case.jpg?raw=true)
![Case Open](img/LANsensor-case-open.jpg?raw=true)
![Board Top](img/LANsensor-board-top.jpg?raw=true)
![Board Bottom](img/LANsensor-board-bottom.jpg?raw=true)


## Usage Notes

### DHCP 
Sensor is meant to be configured using DHCP, in addition to basic network configuration, time/timezone and logging (syslog) server can be configured using DHCP. Device looks for following DHCP options:

* 2 - Time Offset
* 7 - Log Servers (syslog server IP)
* 42 - NTP Servers (NTP server IP)
* 100 - Timezone (current timezone specified as POSIX TZ sting)

### Web Interface

Sensor has basic Web UI available using

```
http://x.x.x.x/
```

Additionally device status is available in CSV format using following url:

```
http://x.x.x.x/raw
```

This returns following string

```
LANsensor,temperature,temperature_unit,humidity,humidity_unit,timestamp,uptime(ticks),devicebootcount
```

Device name can be set using following URL

```
http://x.x.x.x/update?name=Garage
```

Where "Garage" is the name for the Device

### Syslog Output

If syslog server IP address is specified (using DHCP Option 7), then temperature and humidity readings are periodically sent to that server in following format:

```
Sep 29 22:43:28 10.30.42.3 LANsensor: 32.44,C,37.6,%,Garage,2017-09-29 22:43:28 PDT,732903328,37
Sep 29 22:48:28 10.30.42.3 LANsensor: 32.40,C,37.7,%,Garage,2017-09-29 22:48:28 PDT,733203329,37
Sep 29 22:53:27 10.30.42.3 LANsensor: 32.32,C,37.9,%,Garage,2017-09-29 22:53:27 PDT,733503330,37
```


### Compiling

This was built/tested around Arduino Nano, but this should run easily on pretty much any Arduino with minor changes needed.

Following Libraries are needed/used:

* [TinyTZ](https://github.com/tjko/TinyTZ)
* [WireScan](https://github.com/tjko/WireScan)
* [HDC1050](https://github.com/tjko/HDC1050)
* [ethercard](https://github.com/tjko/ethercard)

Note, modified *ethercard* and *HDC1050* libraries were used and using the original versions of these might
require changes to LANsensor code...


