// COMP-10184 – Mohawk College
// Server Monitor Project
//
// Makes a https connection to the CSUnix endpoint and retrieves server info, then makes that info available on a webserver hosted site.
// Also publishes all retrieved data to thingspeak so historical results are available
//
// @author Cole Anderson
//
#include <Arduino.h>
#include <Adafruit_AHTX0.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <CertStoreBearSSL.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <ArduinoJson.h>

//contains wifi *ssid and *password
#include "wifi.h"
//contains writeURL
#include "ts.h"
//contains CSUNIX_ENDPOINT
#include "endpoint.h"

Adafruit_AHTX0 aht;

sensors_event_t humidity, temp;

WiFiUDP ntpUDP;

NTPClient ntpClient(ntpUDP, "ca.pool.ntp.org", -18000);

WiFiClientSecure wifiClient;

BearSSL::CertStore certStore;

ESP8266WebServer webServer(80);

int fieldUpdate = 1;

void handleWebRequests();
void handleDataReq();

void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  Serial.println("File System Initialized");

  Serial.println("Finding temp/humidity sensor...");
  if (!aht.begin()) {
    Serial.println("Could not find AHT? Check wiring");
    // no point in continuing
    ESP.deepSleep(0);
  }
  Serial.println("Found!");

  // fetch root CA's from filesystem and load into certificate store.
  unsigned long startTime = millis();
  int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
  Serial.printf("Read %d CA certs into store in %lu ms\n", numCerts, millis() - startTime);
  if (numCerts == 0) {
    Serial.println("No certs found! Did you upload certs.ar to LittleFS?");
    // no point in continuing
    ESP.deepSleep(0);
  }
  wifiClient.setCertStore(&certStore);

  wifiClient.setSSLVersion(BR_TLS12, BR_TLS12);
  
  // make wifi connection
  Serial.println("Connecting to: " + String(ssid));
  WiFi.begin(ssid, password);

  while ( WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connection established");

  ntpClient.begin();

  //handler for data requests
  webServer.on("/getData", handleDataReq);

  // send all other web requests here by default.  
  // The filesystem will be searched for the requested resource
  webServer.onNotFound(handleWebRequests);  

  webServer.begin();
  Serial.printf("\nWeb server started, open %s in a web browser\n", WiFi.localIP().toString().c_str());
}

//makes an https get request, returns "Failed!" if the response code is not 200, otherwise returns the response
String httpsGetRequest(String endpoint){
  HTTPClient httpClient;
  httpClient.begin(wifiClient, endpoint);
  Serial.println("\nContacting server at: " + String(endpoint));
  int respCode = httpClient.GET();
  String response;
  Serial.printf("HTTP Response Code: %d\n", respCode);
  if ( respCode == HTTP_CODE_OK ) {
    response = httpClient.getString();
  } else if (respCode > 0) {
    Serial.println("Made a secure connection. Server or URL problems?");
    response = "Failed!";
  } else {
    Serial.println("Can't make a secure connection to external server :-(");
    response = "Failed!";
  }
  httpClient.end();

  return response;
}

//updates the ntp time for accurate cert checking
void updateTime() {
  if ( ntpClient.update() ) {
    // need to know the current time to validate a certificate.
    wifiClient.setX509Time(ntpClient.getEpochTime());
    //Serial.println("\nTime set: " + ntpClient.getFormattedTime());
  }
}

//updates all 4 thingspeak fields once a minute, one update on a field every 15 seconds
void updateThingSpeak(){
  aht.getEvent(&humidity, &temp);

  String response = httpsGetRequest(CSUNIX_ENDPOINT);

  if (response != "Failed!"){
    aht.getEvent(&humidity, &temp);

    JsonDocument doc;
    deserializeJson(doc, response);

    String fieldValue;

    switch (fieldUpdate)
    {
      case 1:
        fieldValue = String(((((int(doc["uptime"]["days"]) * 24) + int(doc["uptime"]["hours"])) * 60) + int(doc["uptime"]["minutes"])) * 60);
        break;
      case 2:
        fieldValue = String(doc["memory"]["available"]).substring(0, String(doc["memory"]["available"]).length()-2);
        break;
      case 3:
        fieldValue = String(doc["cpu_loading"]["last_minute"]);
        Serial.println(fieldValue);
        break;
      case 4:
        fieldValue = String(temp.temperature);
        break;
      default:
        break;
    }

    httpsGetRequest(String(writeURL) + "&field" + String(fieldUpdate) + "=" + fieldValue);

    if (fieldUpdate != 4){
      fieldUpdate += 1;
    }else{
      fieldUpdate = 1;
    }
  }
}

//handles requests for csunix server data, returns results in json format
void handleDataReq(){
  aht.getEvent(&humidity, &temp);

  String response = httpsGetRequest(CSUNIX_ENDPOINT);

  if (response != "Failed!"){
    String tempJson = ", \"temperature\": {\"temp\":" + String(temp.temperature) + ",\"humidity\": "+ String(humidity.relative_humidity) +"} }";

    String jsonString = response.substring(0, response.length()-3) + tempJson;

    webServer.send(200, "application/json", jsonString);
  }else{
    webServer.send(400);
  }
}

// *********************************************************************
// this function examines the URL from the client and based on the extension
// determines the type of response to send.
bool loadFromLittleFS(String path) {
  bool bStatus;
  String contentType;
  
  // set bStatus to false assuming this will not work.
  bStatus = false;

  // assume this will be the content type returned, unless path extension 
  // indicates something else
  contentType = "text/plain";

  // DEBUG:  print request URI to user:
  Serial.print("Requested URI: ");
  Serial.println(path.c_str());

  // if no path extension is given, assume index.html is requested.
  if(path.endsWith("/")) path += "index.html";
 
  // look at the URI extension to determine what kind of data to 
  // send to client.
  if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
  else if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".htm"))  contentType = "text/html";
  else if (path.endsWith(".css"))  contentType = "text/css";
  else if (path.endsWith(".js"))   contentType = "application/javascript";
  else if (path.endsWith(".png"))  contentType = "image/png";
  else if (path.endsWith(".gif"))  contentType = "image/gif";
  else if (path.endsWith(".jpg"))  contentType = "image/jpeg";
  else if (path.endsWith(".ico"))  contentType = "image/x-icon";
  else if (path.endsWith(".xml"))  contentType = "text/xml";
  else if (path.endsWith(".pdf"))  contentType = "application/pdf";
  else if (path.endsWith(".zip"))  contentType = "application/zip";

  // try to open file in LittleFS filesystem
  File dataFile = LittleFS.open(path.c_str(), "r");
  // if dataFile <> 0, then it was opened successfully.
  if ( dataFile ) {
    if (webServer.hasArg("download")) contentType = "application/octet-stream";
    // stream the file to the client.  check that it was completely sent.
    if (webServer.streamFile(dataFile, contentType) != dataFile.size()) {
      Serial.println("Error streaming file: " + String(path.c_str()));
    }
    // close the file
    dataFile.close();
    // indicate success
    bStatus = true;
  }
 
  return bStatus;
}

void handleWebRequests(){
  if (!loadFromLittleFS(webServer.uri())) {
    // file not found.  Send 404 response code to client.
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += webServer.uri();
    message += "\nMethod: ";
    message += (webServer.method() == HTTP_GET) ? "GET":"POST";
    message += "\nArguments: ";
    message += webServer.args();
    message += "\n";
    for (uint8_t i=0; i<webServer.args(); i++){
      message += " NAME:"+webServer.argName(i) + "\n VALUE:" + webServer.arg(i) + "\n";
    }
    webServer.send(404, "text/plain", message);
    Serial.println(message);
  }
}

void loop() {
  updateTime();
  
  if (ntpClient.getSeconds()%15 == 0){
    updateThingSpeak();
  }

  webServer.handleClient();
}