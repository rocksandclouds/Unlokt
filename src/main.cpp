/***************************************************
  Main of FingerprintDoorbell
 ****************************************************/

#include <WiFi.h>
#include <DNSServer.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncHTTPUpdateServer.h>
#include <SPIFFS.h>
#include <PubSubClient.h>
#include "FingerprintManager.h"
#include "SettingsManager.h"
#include "tone.h"
#include "global.h"

enum class Mode
{
  scan,
  enroll,
  wificonfig,
  maintenance
};

const char *VersionInfo = "0.5";

// ===================================================================================================================
// Caution: below are not the credentials for connecting to your home network, they are for the Access Point mode!!!
// ===================================================================================================================
const char *WifiConfigSsid = "FingerprintDoorbell-Config"; // SSID used for WiFi when in Access Point mode for configuration
const char *WifiConfigPassword = "12345678";               // password used for WiFi when in Access Point mode for configuration. Min. 8 chars needed!
IPAddress WifiConfigIp(192, 168, 4, 1);                    // IP of access point in wifi config mode

const char *Selected = "selected"; // password used for WiFi when in Access Point mode for configuration. Min. 8 chars needed!
const char *Checked = "checked";   // password used for WiFi when in Access Point mode for configuration. Min. 8 chars needed!

const long gmtOffset_sec = 0;       // UTC Time
const int daylightOffset_sec = 0;   // UTC Time
const int doorbellOutputPin = 19;   // pin connected to the doorbell (when using hardware connection instead of mqtt to ring the bell)
const int dooropenerOutputPin = 23; // pin connected to the door opener (when using hardware connection instead of mqtt to open the door)

const int statusLed = LED_BUILTIN;

#ifdef CUSTOM_GPIOS
const int customOutput1 = 18; // not used internally, but can be set over MQTT
const int customOutput2 = 26; // not used internally, but can be set over MQTT
const int customInput1 = 21;  // not used internally, but changes are published over MQTT
const int customInput2 = 22;  // not used internally, but changes are published over MQTT
bool customInput1Value = false;
bool customInput2Value = false;
#endif

const int logMessagesCount = 5;
String logMessages[logMessagesCount]; // log messages, 0=most recent log message
bool shouldReboot = false;
unsigned long wifiReconnectPreviousMillis = 0;
unsigned long mqttReconnectPreviousMillis = 0;

String enrollId;
String enrollName;
Mode currentMode = Mode::scan;

FingerprintManager fingerManager;
SettingsManager settingsManager;
bool needMaintenanceMode = false;

const byte DNS_PORT = 53;
DNSServer dnsServer;
ESPAsyncHTTPUpdateServer updateServer;
AsyncWebServer webServer(80);       // AsyncWebServer  on port 80
AsyncEventSource events("/events"); // event source (Server-Sent events)


const char *webServerUser = "admin"; // User name for authentication
#define MQTT_LWT_TOPIC "LWT"
#define MQTT_LWT_RETAIN true
#define MQTT_LWT_QOS 2
#define MQTT_LWT_PAYLOAD_ONLINE "Online"
#define MQTT_LWT_PAYLOAD_OFFLINE "Offline"

WiFiClient espClient;
PubSubClient mqttClient(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
bool mqttConfigValid = true;

Match lastMatch;

void addLogMessage(const String &message)
{
  // shift all messages in array by 1, oldest message will die
  for (int i = logMessagesCount - 1; i > 0; i--)
    logMessages[i] = logMessages[i - 1];
  logMessages[0] = message;
}

String getLogMessagesAsHtml()
{
  String html = "";
  for (int i = logMessagesCount - 1; i >= 0; i--)
  {
    if (logMessages[i] != "")
      html = html + logMessages[i] + "<br>";
  }
  return html;
}

String getTimestampString()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time");
    return "no time";
  }

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  String datetime = String(buffer);
  return datetime;
}

String getUptimeString()
{
  unsigned long milli = millis();
  unsigned long secs=milli/1000, mins=secs/60;
  unsigned int hours=mins/60, days=hours/24;
  milli-=secs*1000;
  secs-=mins*60;
  mins-=hours*60;
  hours-=days*24;
  char buffer[25];
  snprintf(buffer, sizeof(buffer), "Uptime %d days %2.2d:%2.2d:%2.2d", (byte)days, (byte)hours, (byte)mins, (byte)secs);
  String uptime = String(buffer);
  return uptime;
}

String splitIpAndPort(String mqttServerConfigString, u16_t &mqttPort)
{
  String mqttHostname;
  s8_t positionOfColon = mqttServerConfigString.indexOf(":");

  if (positionOfColon >= 0)
  {
    mqttHostname = mqttServerConfigString.substring(0, positionOfColon);
    mqttPort = mqttServerConfigString.substring(positionOfColon + 1).toInt();
  }
  else
  {
    mqttHostname = mqttServerConfigString;
    mqttPort = 1883;
  }
  return mqttHostname;
}

/* wait for maintenance mode or timeout 5s */
bool waitForMaintenanceMode()
{
  needMaintenanceMode = true;
  unsigned long startMillis = millis();
  while (currentMode != Mode::maintenance)
  {
    if ((millis() - startMillis) >= 5000ul)
    {
      needMaintenanceMode = false;
      return false;
    }
    delay(50);
  }
  needMaintenanceMode = false;
  return true;
}

// Replaces placeholder in HTML pages
String processor(const String &var)
{
  if (var == "LOGMESSAGES")
  {
    return getLogMessagesAsHtml();
  }
  else if (var == "FINGERLIST")
  {
    return fingerManager.getFingerListAsHtmlOptionList();
  }
  else if (var == "HOSTNAME")
  {
    return settingsManager.getWifiSettings().hostname;
  }
  else if (var == "VERSIONINFO")
  {
    return VersionInfo;
  }
  else if (var == "IP_ADDRESS")
  {
    return WiFi.localIP().toString();
  }
  else if (var == "MAC_ADDRESS")
  {
    return WiFi.macAddress();
  }
  else if (var == "SSID")
  {
    return WiFi.SSID();
  }
  else if (var == "BSSID")
  {
    return WiFi.BSSIDstr();
  }
  else if (var == "WIFI_CHANNEL")
  {
    return String(WiFi.channel());
  }
  else if (var == "RSSI")
  {
    return String(WiFi.RSSI()) + " dBm";
  }
  else if (var == "GATEWAY")
  {
    return WiFi.gatewayIP().toString();
  }
  else if (var == "DNS_SERVER")
  {
    return WiFi.dnsIP().toString();
  }
  else if (var == "MQTT_SERVER_IP")
  {
    IPAddress mqttServerIp;
    u16_t mqttPort;
    String mqttHostname = splitIpAndPort(settingsManager.getAppSettings().mqttServer, mqttPort);
    if (WiFi.hostByName(mqttHostname.c_str(), mqttServerIp))
    {
      return mqttServerIp.toString();
    }
  }
  else if (var == "CHIP")
  {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    return String(ESP.getChipModel()) + String(" rev:") + String(chip_info.revision);
  }
  else if (var == "CPU")
  {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    return String(ESP.getCpuFreqMHz()) + String(" Mhz ") + String(chip_info.cores) + String(" Core(s)");
  }
  else if (var == "CHIP_TEMP")
  {
    return String(round(temperatureRead() * 10) / 10) + String(" °C");
  }
  else if (var == "FLASH_SIZE")
  {
    return String(ESP.getFlashChipSize());
  }
  else if (var == "FREE_HEAP")
  {
    return String(ESP.getFreeHeap());
  }
  else if (var == "RESET_REASON")
  {
    switch (esp_reset_reason())
    {
      case ESP_RST_UNKNOWN:
          return String("Unknown");
      case ESP_RST_POWERON:
          return String("Power on");
      case ESP_RST_EXT:
          return String("External");
      case ESP_RST_SW:
          return String("Software");
      case ESP_RST_PANIC:
          return String("Panic");
      case ESP_RST_INT_WDT:
          return String("Interrupt Watchdog");
      case ESP_RST_TASK_WDT:
          return String("Task Watchdog");
      case ESP_RST_WDT:
          return String("Watchdog");
      case ESP_RST_DEEPSLEEP:
          return String("Deepsleep");
      case ESP_RST_BROWNOUT:
          return String("Brownout");
      case ESP_RST_SDIO:
          return String("SDIO");
    }
  }
  else if (var == "UPTIME")
  {
    return getUptimeString();
  }
  else if (var == "WIFI_SSID")
  {
    return settingsManager.getWifiSettings().ssid;
  }
  else if (var == "WIFI_PASSWORD")
  {
    if (settingsManager.getWifiSettings().password.isEmpty())
      return "";
    else
      return "********"; // for security reasons the wifi password will not left the device once configured
  }
  else if (var == "WEB_PASSWORD")
  {
    if (settingsManager.getAppSettings().webPassword.isEmpty())
      return "";
    else
      return "********"; // for security reasons the web password will not left the device once configured
  }
  else if (var == "MQTT_SERVER")
  {
    return settingsManager.getAppSettings().mqttServer;
  }
  else if (var == "MQTT_USERNAME")
  {
    return settingsManager.getAppSettings().mqttUsername;
  }
  else if (var == "MQTT_PASSWORD")
  {
    return settingsManager.getAppSettings().mqttPassword;
  }
  else if (var == "MQTT_ROOTTOPIC")
  {
    return settingsManager.getAppSettings().mqttRootTopic;
  }
  else if (var == "NTP_SERVER")
  {
    return settingsManager.getAppSettings().ntpServer;
  }
  else if (var == "TIMEZONE")
  {
    return settingsManager.getAppSettings().timezone;
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_1")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 1) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_2")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 2) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_3")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 3) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_4")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 4) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_5")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 5) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_6")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 6) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_COLOR_7")
  {
    return (settingsManager.getAppSettings().touchRingActiveColor == 7) ? Selected : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_SEQUENCE_4")
  {
    return (settingsManager.getAppSettings().touchRingActiveSequence == 4) ? Checked : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_SEQUENCE_3")
  {
    return (settingsManager.getAppSettings().touchRingActiveSequence == 3) ? Checked : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_SEQUENCE_1")
  {
    return (settingsManager.getAppSettings().touchRingActiveSequence == 1) ? Checked : "";
  }
  else if (var == "TOUCH_RING_ACTIVE_SEQUENCE_2")
  {
    return (settingsManager.getAppSettings().touchRingActiveSequence == 2) ? Checked : "";
  }
  else if (var == "SCAN_COLOR_1")
  {
    return (settingsManager.getAppSettings().scanColor == 1) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_2")
  {
    return (settingsManager.getAppSettings().scanColor == 2) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_3")
  {
    return (settingsManager.getAppSettings().scanColor == 3) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_4")
  {
    return (settingsManager.getAppSettings().scanColor == 4) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_5")
  {
    return (settingsManager.getAppSettings().scanColor == 5) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_6")
  {
    return (settingsManager.getAppSettings().scanColor == 6) ? Selected : "";
  }
  else if (var == "SCAN_COLOR_7")
  {
    return (settingsManager.getAppSettings().scanColor == 7) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_1")
  {
    return (settingsManager.getAppSettings().matchColor == 1) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_2")
  {
    return (settingsManager.getAppSettings().matchColor == 2) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_3")
  {
    return (settingsManager.getAppSettings().matchColor == 3) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_4")
  {
    return (settingsManager.getAppSettings().matchColor == 4) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_5")
  {
    return (settingsManager.getAppSettings().matchColor == 5) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_6")
  {
    return (settingsManager.getAppSettings().matchColor == 6) ? Selected : "";
  }
  else if (var == "MATCH_COLOR_7")
  {
    return (settingsManager.getAppSettings().matchColor == 7) ? Selected : "";
  }

  return String();
}

// send LastMessage to websocket clients
void notifyClients(String message)
{
  String messageWithTimestamp = "[" + getTimestampString() + "]: " + message;
  Serial.println(messageWithTimestamp);
  addLogMessage(messageWithTimestamp);
  events.send(getLogMessagesAsHtml().c_str(), "message", millis(), 1000);

  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  mqttClient.publish((String(mqttRootTopic) + "/lastLogMessage").c_str(), message.c_str());
}

void updateClientsFingerlist(String fingerlist)
{
  Serial.println("New fingerlist was sent to clients");
  events.send(fingerlist.c_str(), "fingerlist", millis(), 1000);
}

bool doPairing()
{
  String newPairingCode = settingsManager.generateNewPairingCode();

  if (fingerManager.setPairingCode(newPairingCode))
  {
    AppSettings settings = settingsManager.getAppSettings();
    settings.sensorPairingCode = newPairingCode;
    settings.sensorPairingValid = true;
    settingsManager.saveAppSettings(settings);
    notifyClients("Pairing successful.");
    return true;
  }
  else
  {
    notifyClients("Pairing failed.");
    return false;
  }
}

bool checkPairingValid()
{
  AppSettings settings = settingsManager.getAppSettings();

  if (!settings.sensorPairingValid)
  {
    if (settings.sensorPairingCode.isEmpty())
    {
      // first boot, do pairing automatically so the user does not have to do this manually
      return doPairing();
    }
    else
    {
      Serial.println("Pairing has been invalidated previously.");
      return false;
    }
  }

  String actualSensorPairingCode = fingerManager.getPairingCode();
  // Serial.println("Awaited pairing code: " + settings.sensorPairingCode);
  // Serial.println("Actual pairing code: " + actualSensorPairingCode);

  if (actualSensorPairingCode.equals(settings.sensorPairingCode))
    return true;
  else
  {
    if (!actualSensorPairingCode.isEmpty())
    {
      // An empty code means there was a communication problem. So we don't have a valid code, but maybe next read will succeed and we get one again.
      // But here we just got an non-empty pairing code that was different to the awaited one. So don't expect that will change in future until repairing was done.
      // -> invalidate pairing for security reasons
      AppSettings settings = settingsManager.getAppSettings();
      settings.sensorPairingValid = false;
      settingsManager.saveAppSettings(settings);
    }
    return false;
  }
}

bool initWifi()
{
  // Connect to Wi-Fi
  WifiSettings wifiSettings = settingsManager.getWifiSettings();
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(wifiSettings.hostname.c_str()); // define hostname
  WiFi.begin(wifiSettings.ssid.c_str(), wifiSettings.password.c_str());
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Waiting for WiFi connection...");
    counter++;
    if (counter > 30)
      return false;
  }
  Serial.println("Connected!");

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());

  return true;
}

void initWiFiAccessPointForConfiguration()
{
  WiFi.softAPConfig(WifiConfigIp, WifiConfigIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WifiConfigSsid, WifiConfigPassword);

  // if DNSServer is started with "*" for domain name, it will reply with
  // provided IP to all DNS request
  dnsServer.start(DNS_PORT, "*", WifiConfigIp);

  Serial.print("AP IP address: ");
  Serial.println(WifiConfigIp);
}

void startWebserver()
{

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Init time by NTP Client
  configTime(gmtOffset_sec, daylightOffset_sec, settingsManager.getAppSettings().ntpServer.c_str());
  Serial.printf("Setting Timezone to %s\n", settingsManager.getAppSettings().timezone.c_str());
  setenv("TZ", settingsManager.getAppSettings().timezone.c_str(), 1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();

  // webserver for normal operating or wifi config?
  if (currentMode == Mode::wificonfig)
  {
    // =================
    // WiFi config mode
    // =================

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                 { request->send(SPIFFS, "/wificonfig.html", String(), false, processor); });

    webServer.on("/save", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if(request->hasArg("hostname"))
      {
        Serial.println("Save wifi config");
        WifiSettings settings = settingsManager.getWifiSettings();
        settings.hostname = request->arg("hostname");
        settings.ssid = request->arg("ssid");
        if (request->arg("password").equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
          settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
        else
          settings.password = request->arg("password");
        settingsManager.saveWifiSettings(settings);
        shouldReboot = true;
      }
      request->redirect("/"); });

    webServer.onNotFound([](AsyncWebServerRequest *request)
                         {
      AsyncResponseStream *response = request->beginResponseStream("text/html");
      response->printf("<!DOCTYPE html><html><head><title>FingerprintDoorbell</title><meta http-equiv=\"refresh\" content=\"0; url=http://%s\" /></head><body>", WiFi.softAPIP().toString().c_str());
      response->printf("<p>Please configure your WiFi settings <a href='http://%s'>here</a> to connect FingerprintDoorbell to your home network.</p>", WiFi.softAPIP().toString().c_str());
      response->print("</body></html>");
      request->send(response); });
  }
  else
  {
    // =======================
    // normal operating mode
    // =======================
    events.onConnect([](AsyncEventSourceClient *client)
                     {
      if(client->lastId()){
        Serial.printf("Client reconnected! Last message ID it got was: %u\n", client->lastId());
      }
      //send event with message "ready", id current millis
      // and set reconnect delay to 1 second
      client->send(getLogMessagesAsHtml().c_str(),"message",millis(),1000); });
    webServer.addHandler(&events);

    // Route for root / web page
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      request->send(SPIFFS, "/index.html", String(), false, processor); });

    webServer.on("/enroll", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      if(request->hasArg("startEnrollment"))
      {
        enrollId = request->arg("newFingerprintId");
        enrollName = request->arg("newFingerprintName");
        currentMode = Mode::enroll;
      }
      request->redirect("/"); });

    webServer.on("/editFingerprints", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      if(request->hasArg("selectedFingerprint"))
      {
        if(request->hasArg("btnDelete"))
        {
          int id = request->arg("selectedFingerprint").toInt();
          waitForMaintenanceMode();
          fingerManager.deleteFinger(id);
          currentMode = Mode::scan;
        }
        else if (request->hasArg("btnRename"))
        {
          int id = request->arg("selectedFingerprint").toInt();
          String newName = request->arg("renameNewName");
          fingerManager.renameFinger(id, newName);
        }
      }
      request->redirect("/"); });

    webServer.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }

      if(request->hasArg("btnSaveSettings"))
      {
        Serial.println("Save settings");
        AppSettings settings = settingsManager.getAppSettings();
        if (request->arg("web_password").equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
          settings.webPassword = settingsManager.getAppSettings().webPassword; // use the old, already saved, one
        else
          settings.webPassword = request->arg("web_password");
        settings.mqttServer = request->arg("mqtt_server");
        settings.mqttUsername = request->arg("mqtt_username");
        settings.mqttPassword = request->arg("mqtt_password");
        settings.mqttRootTopic = request->arg("mqtt_rootTopic");
        settings.ntpServer = request->arg("ntpServer");
        settings.touchRingActiveColor = request->arg("touchRingActiveColor").toInt();
        settings.touchRingActiveSequence = request->arg("touchRingActiveSequence").toInt();
        settings.scanColor = request->arg("scanColor").toInt();
        settings.matchColor = request->arg("matchColor").toInt();
        settingsManager.saveAppSettings(settings);
        request->redirect("/");  
        shouldReboot = true;
      } else {
        request->send(SPIFFS, "/settings.html", String(), false, processor);
      } });

    webServer.on("/system", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      request->send(SPIFFS, "/system.html", String(), false, processor);
      });

    webServer.on("/pairing", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      if(request->hasArg("btnDoPairing"))
      {
        Serial.println("Do (re)pairing");
        doPairing();
        request->redirect("/");  
      } else {
        request->send(SPIFFS, "/settings.html", String(), false, processor);
      } });

    webServer.on("/factoryReset", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      if(request->hasArg("btnFactoryReset"))
      {
        notifyClients("Factory reset initiated...");
        
        if (!fingerManager.deleteAll())
          notifyClients("Finger database could not be deleted.");
        
        if (!settingsManager.deleteAppSettings())
          notifyClients("App settings could not be deleted.");

        if (!settingsManager.deleteWifiSettings())
          notifyClients("Wifi settings could not be deleted.");
        
        request->redirect("/");  
        shouldReboot = true;
      } else {
        request->send(SPIFFS, "/settings.html", String(), false, processor);
      } });

    webServer.on("/deleteAllFingerprints", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
      if (!settingsManager.getAppSettings().webPassword.isEmpty())
      {
        if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
          return request->requestAuthentication();
      }
      if(request->hasArg("btnDeleteAllFingerprints"))
      {
        notifyClients("Deleting all fingerprints...");
        
        if (!fingerManager.deleteAll())
          notifyClients("Finger database could not be deleted.");
        
        request->redirect("/");  
        
      }
      else
      {
        if (!settingsManager.getAppSettings().webPassword.isEmpty())
        {
          if(!request->authenticate(webServerUser, settingsManager.getAppSettings().webPassword.c_str()))
            return request->requestAuthentication();
        }
        request->send(SPIFFS, "/settings.html", String(), false, processor);
      } });

    webServer.onNotFound([](AsyncWebServerRequest *request)
                         { request->send(404); });

  } // end normal operating mode

  // common url callbacks
  webServer.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request)
               {
    request->redirect("/");
    shouldReboot = true; });

  webServer.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(SPIFFS, "/bootstrap.min.css", "text/css"); });



  // Start server
  updateServer.setup(&webServer);
  webServer.begin();
  if (settingsManager.getAppSettings().webPassword.isEmpty())
    updateServer.setup(&webServer, "/update");
  else
    updateServer.setup(&webServer, "/update", webServerUser, settingsManager.getAppSettings().webPassword.c_str());

  notifyClients("System booted successfully!");
}

void mqttCallback(char *topic, byte *message, unsigned int length)
{
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;

  for (int i = 0; i < length; i++)
  {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Check incomming message for interesting topics
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/ignoreTouchRing")
  {
    if (messageTemp == "on")
    {
      fingerManager.setIgnoreTouchRing(true);
    }
    else if (messageTemp == "off")
    {
      fingerManager.setIgnoreTouchRing(false);
    }
  }

  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/openDoor")
  {
    if (messageTemp == "true")
    {
      digitalWrite(dooropenerOutputPin, HIGH);
      Serial.println("MQTT message received: Open the door!");
      delay(1000);
      digitalWrite(dooropenerOutputPin, LOW);
    }
  }

#ifdef CUSTOM_GPIOS
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput1")
  {
    if (messageTemp == "on")
    {
      digitalWrite(customOutput1, HIGH);
    }
    else if (messageTemp == "off")
    {
      digitalWrite(customOutput1, LOW);
    }
  }
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput2")
  {
    if (messageTemp == "on")
    {
      digitalWrite(customOutput2, HIGH);
    }
    else if (messageTemp == "off")
    {
      digitalWrite(customOutput2, LOW);
    }
  }
#endif
}

void connectMqttClient()
{
  if (!mqttClient.connected() && mqttConfigValid)
  {
    Serial.print("(Re)connect to MQTT broker...");
    // Attempt to connect
    bool connectResult;

    // connect with or witout authentication
    String lastWillTopic = settingsManager.getAppSettings().mqttRootTopic + "/" + MQTT_LWT_TOPIC;
    if (settingsManager.getAppSettings().mqttUsername.isEmpty() || settingsManager.getAppSettings().mqttPassword.isEmpty())
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(), lastWillTopic.c_str(), MQTT_LWT_QOS, MQTT_LWT_RETAIN, MQTT_LWT_PAYLOAD_OFFLINE);
    else
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(), settingsManager.getAppSettings().mqttUsername.c_str(), settingsManager.getAppSettings().mqttPassword.c_str(), lastWillTopic.c_str(), MQTT_LWT_QOS, MQTT_LWT_RETAIN, MQTT_LWT_PAYLOAD_OFFLINE);

    if (connectResult)
    {
      // success
      Serial.println("connected");
      tone(2000, 200); // play a tone to indicate success
      // LWT publish
      mqttClient.publish((settingsManager.getAppSettings().mqttRootTopic + "/" + MQTT_LWT_TOPIC).c_str(), MQTT_LWT_PAYLOAD_ONLINE, MQTT_LWT_RETAIN);
      // Subscribe
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/ignoreTouchRing").c_str(), 1); // QoS = 1 (at least once)
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/openDoor").c_str(), 1); // QoS = 1 (at least once)
#ifdef CUSTOM_GPIOS
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput1").c_str(), 1); // QoS = 1 (at least once)
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput2").c_str(), 1); // QoS = 1 (at least once)
#endif
    }
    else
    {
      if (mqttClient.state() == 4 || mqttClient.state() == 5)
      {
        mqttConfigValid = false;
        notifyClients("Failed to connect to MQTT Server: bad credentials or not authorized. Will not try again, please check your settings.");
      }
      else
      {
        notifyClients(String("Failed to connect to MQTT Server, rc=") + mqttClient.state() + ", try again in 30 seconds");
      }
    }
  }
}

void doScan()
{
  Match match = fingerManager.scanFingerprint();
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  switch (match.scanResult)
  {
  case ScanResult::noFinger:
    // standard case, occurs every iteration when no finger touchs the sensor
    if (match.scanResult != lastMatch.scanResult)
    {
      Serial.println("no finger");
      mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
      mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
      mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
      mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
    }
    break;
  case ScanResult::matchFound:
    notifyClients(String("Match Found: ") + match.matchId + " - " + match.matchName + " with confidence of " + match.matchConfidence);
    if (match.scanResult != lastMatch.scanResult)
    {
      if (checkPairingValid())
      {
        bool opendoor = false;
        if (match.matchName.endsWith("#"))
          opendoor = true;
        if (opendoor)
          digitalWrite(dooropenerOutputPin, HIGH);        mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), String(match.matchId).c_str());
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), match.matchName.c_str());
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), String(match.matchConfidence).c_str());
        Serial.println("MQTT message sent: Open the door!");
        if (opendoor)
        {
          delay(1000);
          digitalWrite(dooropenerOutputPin, LOW);
        }
      }
      else
      {
        notifyClients("Security issue! Match was not sent by MQTT because of invalid sensor pairing! This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page.");
      }
    }
    delay(3000); // wait some time before next scan to let the LED blink
    break;
  case ScanResult::noMatchFound:
    notifyClients(String("No Match Found (Code ") + match.returnCode + ")");
    if (match.scanResult != lastMatch.scanResult)
    {
      digitalWrite(doorbellOutputPin, HIGH);
      mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "on");
      mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
      mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
      mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
      Serial.println("MQTT message sent: ring the bell!");
      delay(1000);
      digitalWrite(doorbellOutputPin, LOW);
      tone(600, 150);
      tone(400, 200);
    }
    else
    {
      delay(1000); // wait some time before next scan to let the LED blink
    }
    break;
  case ScanResult::error:
    notifyClients(String("ScanResult Error (Code ") + match.returnCode + ")");
    break;
  };
  lastMatch = match;
}

void doEnroll()
{
  int id = enrollId.toInt();
  if (id < 1 || id > 200)
  {
    notifyClients("Invalid memory slot id '" + enrollId + "'");
    return;
  }

  NewFinger finger = fingerManager.enrollFinger(id, enrollName);
  if (finger.enrollResult == EnrollResult::ok)
  {
    notifyClients("Enrollment successfull. You can now use your new finger for scanning.");
    updateClientsFingerlist(fingerManager.getFingerListAsHtmlOptionList());
  }
  else if (finger.enrollResult == EnrollResult::error)
  {
    notifyClients(String("Enrollment failed. (Code ") + finger.returnCode + ")");
  }
}

void reboot()
{
  notifyClients("System is rebooting now...");
  delay(1000);

  mqttClient.disconnect();
  espClient.stop();
  dnsServer.stop();
  webServer.end();
  WiFi.disconnect();
  ESP.restart();
}

void setup()
{
  // open serial monitor for debug infos
  Serial.begin(115200);
  while (!Serial)
    ; // For Yun/Leo/Micro/Zero/...
  delay(100);

  // initialize GPIOs
  pinMode(doorbellOutputPin, OUTPUT);
  digitalWrite(doorbellOutputPin, LOW);
  pinMode(dooropenerOutputPin, OUTPUT);
  digitalWrite(dooropenerOutputPin, LOW);
#ifdef CUSTOM_GPIOS
  pinMode(customOutput1, OUTPUT);
  pinMode(customOutput2, OUTPUT);
  pinMode(customInput1, INPUT_PULLDOWN);
  pinMode(customInput2, INPUT_PULLDOWN);
#endif

  settingsManager.loadWifiSettings();
  settingsManager.loadAppSettings();

  fingerManager.connect();

  if (!checkPairingValid())
    notifyClients("Security issue! Pairing with sensor is invalid. This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page. MQTT messages regarding matching fingerprints will not been sent until pairing is valid again.");

  if (fingerManager.isFingerOnSensor() || !settingsManager.isWifiConfigured())
  {
    // ring touched during startup or no wifi settings stored -> wifi config mode
    currentMode = Mode::wificonfig;
    Serial.println("Started WiFi-Config mode");
    fingerManager.setLedRingWifiConfig();
    initWiFiAccessPointForConfiguration();
    startWebserver();
  }
  else
  {
    Serial.println("Started normal operating mode");
    currentMode = Mode::scan;
    if (initWifi())
    {
      startWebserver();
      if (settingsManager.getAppSettings().mqttServer.isEmpty())
      {
        mqttConfigValid = false;
        notifyClients("Error: No MQTT Broker is configured! Please go to settings and enter your server URL + user credentials.");
      }
      else
      {
        delay(5000);

        u16_t mqttPort;
        String mqttHostname = splitIpAndPort(settingsManager.getAppSettings().mqttServer, mqttPort);

        IPAddress mqttServerIp;
        if (WiFi.hostByName(mqttHostname.c_str(), mqttServerIp))
        {
          mqttConfigValid = true;
          Serial.println("IP used for MQTT server: " + mqttServerIp.toString() + ":" + mqttPort);
          mqttClient.setServer(mqttServerIp, mqttPort);
          mqttClient.setCallback(mqttCallback);
          connectMqttClient();
        }
        else
        {
          mqttConfigValid = false;
          notifyClients("MQTT Server '" + settingsManager.getAppSettings().mqttServer + "' not found. Please check your settings.");
        }
      }
      if (fingerManager.connected)
      {
        fingerManager.configTouchRingActive(settingsManager.getAppSettings().touchRingActiveColor, settingsManager.getAppSettings().touchRingActiveSequence, settingsManager.getAppSettings().scanColor, settingsManager.getAppSettings().matchColor);
        fingerManager.setLedRingReady();
      }
      else
        fingerManager.setLedRingError();
    }
    else
    {
      fingerManager.setLedRingError();
      shouldReboot = true;
    }
  }
}

void loop()
{
  // shouldReboot flag for supporting reboot through webui
  if (shouldReboot)
  {
    reboot();
  }

  // Reconnect handling
  if (currentMode != Mode::wificonfig)
  {
    unsigned long currentMillis = millis();
    // reconnect WiFi if down for 30s
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - wifiReconnectPreviousMillis >= 30000ul))
    {
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.reconnect();
      wifiReconnectPreviousMillis = currentMillis;
    }

    // reconnect mqtt if down
    if (!settingsManager.getAppSettings().mqttServer.isEmpty())
    {
      if (!mqttClient.connected() && (currentMillis - mqttReconnectPreviousMillis >= 30000ul))
      {
        connectMqttClient();
        mqttReconnectPreviousMillis = currentMillis;
      }
      mqttClient.loop();
    }
  }

  // do the actual loop work
  switch (currentMode)
  {
  case Mode::scan:
    if (fingerManager.connected)
      doScan();
    break;

  case Mode::enroll:
    doEnroll();
    currentMode = Mode::scan; // switch back to scan mode after enrollment is done
    break;

  case Mode::wificonfig:
    dnsServer.processNextRequest(); // used for captive portal redirect
    break;

  case Mode::maintenance:
    // do nothing, give webserver exclusive access to sensor (not thread-safe for concurrent calls)
    break;
  }

  // enter maintenance mode (no continous scanning) if requested
  if (needMaintenanceMode)
    currentMode = Mode::maintenance;

#ifdef CUSTOM_GPIOS
  // read custom inputs and publish by MQTT
  bool i1;
  bool i2;
  i1 = (digitalRead(customInput1) == HIGH);
  i2 = (digitalRead(customInput2) == HIGH);

  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  if (i1 != customInput1Value)
  {
    if (i1)
      mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "on");
    else
      mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "off");
  }

  if (i2 != customInput2Value)
  {
    if (i2)
      mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "on");
    else
      mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "off");
  }

  customInput1Value = i1;
  customInput2Value = i2;

#endif
}
