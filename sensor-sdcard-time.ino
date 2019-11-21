#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <Preferences.h>
#include <Wire.h>
#include <ds3231.h>

Preferences preferences;
char fileName[20]; //yyyyMMddhhmm
bool getTimeOk = false, uploadStringOk;
unsigned long currentTime, syncTime, syncMillis, timeStamp = 0;
hw_timer_t * timer = NULL;
String conductivity, pH, temperature;
TaskHandle_t LiveTH, TimeTH, CloudTH, LoopTH;
SemaphoreHandle_t LiveSH, TimeSH, CloudSH, fileNameSH, sensorsSH, sensorsStringSH;
File sdFile;
byte count = 0;
String sensorsString;

void IRAM_ATTR onTimer() {
  vTaskNotifyGiveFromISR(LoopTH, NULL);
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case SYSTEM_EVENT_STA_DISCONNECTED: //if wifi disconnect, suspend LiveTask and CloudTask
      Serial.println("WiFi Disconnected"); WiFi.reconnect(); //reconnect manually
      vTaskSuspend(LiveTH);
      vTaskSuspend(CloudTH);
      break;
    case SYSTEM_EVENT_STA_GOT_IP: //resume 3 tasks when connected
      Serial.println("WiFi Connected");
      vTaskResume(LiveTH);
      if (count == 0) uploadStringOk = true; else count = 254; //tell CloudTask to stop adding data to request after being suspended
      vTaskResume(CloudTH);
      vTaskResume(TimeTH);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  if (!SD.begin()) {
    Serial.println("SD Card Mount Failed");
    return;
  }
  preferences.begin("my-app", false);
  syncTime = preferences.getUInt("sync_time", 0); //get timeStamp when the DS module was updated from Internet
  Wire1.begin(21, 22); Wire.begin(26, 27);

  TimeSH = xSemaphoreCreateBinary(); LiveSH = xSemaphoreCreateBinary(); CloudSH = xSemaphoreCreateBinary(); LoopTH = xTaskGetCurrentTaskHandle(); //init value: 0
  vSemaphoreCreateBinary(fileNameSH); vSemaphoreCreateBinary(sensorsSH); vSemaphoreCreateBinary(sensorsStringSH);                                 //init value: 1

  vTaskPrioritySet(NULL, 5);                                                //SensorTask - Core 1 - priority 5 (highest)
  xTaskCreate(TimeTask, "time", 2500, NULL, 4, &TimeTH);                    //TimeTask priority 4
  xTaskCreatePinnedToCore(LiveTask, "upload", 3000, NULL, 3, &LiveTH, 1);   //LiveTask - Core 1 - priority 3
  xTaskCreatePinnedToCore(CloudTask, "drive", 24000, NULL, 2, &CloudTH, 0); //CloudTask - Core 0 - priority 2

  WiFi.setAutoReconnect(false); WiFi.onEvent(WiFiEvent); WiFi.begin("Knab", "quachthiducanh"); //connect to WiFi

  DS3231_init(DS3231_CONTROL_INTCN); //get time from DS module
  ts time_DS;
  DS3231_get(&time_DS);
  currentTime = time_DS.unixtime;
  Serial.print("DS3231 time: "); Serial.println(currentTime);
  if (currentTime - syncTime > 7776000 || time_DS.year < 2019) { //3 months, current time is so wrong
    Serial.println("Wait to get time from Internet");
    xSemaphoreGive(TimeSH);
    while (!getTimeOk) {
      delay(500); Serial.println("."); //wait until TimeTask finish
    }
  }
  
  timer = timerBegin(0, 40000, true); //prescaler
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 30000, true); //counter //2000 is one second
  timerAlarmEnable(timer); //Timer fires every 15 seconds
}

void loop() { //SensorTask
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY); //wait until timer notify
  Serial.println("Reading Sensors");
  xSemaphoreTake(sensorsSH, portMAX_DELAY);
  Wire1.beginTransmission(100); Wire1.write('r'); Wire1.endTransmission(); //request sensor readings
  Wire1.beginTransmission(99); Wire1.write('r'); Wire1.endTransmission();
  Wire1.beginTransmission(102); Wire1.write('r'); Wire1.endTransmission();
  delay(790); //wait for the sensors to return results
  Wire1.requestFrom(100, 20); Wire1.read(); conductivity = Wire1.readStringUntil('\0');
  Wire1.requestFrom(99, 20); Wire1.read(); pH = Wire1.readStringUntil('\0');
  Wire1.requestFrom(102, 20); Wire1.read(); temperature = Wire1.readStringUntil('\0');
  xSemaphoreGive(sensorsSH);
  if (getTimeOk) { //update time when TimeTask finished
    getTimeOk = false;
    currentTime = syncTime + (millis() - syncMillis + 500) / 1000; //syncTime is in between timer interrupts
  } else {
    currentTime += 15; //30
  }
  xSemaphoreGive(LiveSH);
  xSemaphoreTake(sensorsStringSH, portMAX_DELAY);
  sensorsString = String(currentTime) + "," + conductivity + "," + pH + "," + temperature + "\n"; //combine sensor data in one string to use later
  xSemaphoreGive(sensorsStringSH);
  if (currentTime >= timeStamp) { //create new file when every 2 minutes
    if (count > 0) count = 254; //signal CloudTask to stop incrementting 'count' after create new file
    tm time_h;
    timeStamp = currentTime - currentTime % 120 + 120; //next even minute timestamp
    time_t x = timeStamp - 120;
    gmtime_r(&x, &time_h);
    xSemaphoreTake(fileNameSH, portMAX_DELAY);
    sprintf(fileName, "/esp32/%d%02d%02d%02d%02d", time_h.tm_year + 1900, time_h.tm_mon + 1, time_h.tm_mday, time_h.tm_hour, time_h.tm_min);
    xSemaphoreGive(fileNameSH);
    sdFile = SD.open(fileName, FILE_APPEND);
    Serial.print("New File: "); Serial.println(fileName);
    xSemaphoreGive(TimeSH);
  }
  xSemaphoreGive(CloudSH);
  sdFile.print(sensorsString); //store sensor data to sd card
  sdFile.flush();
}

void LiveTask(void* param) {
  vTaskSuspend(NULL); //suspend until connected to WiFi
  unsigned long start, now = 0;
  WiFiClient client;
  String payload, request, adafruit = "POST /api/v2/k8C/groups/as/data HTTP/1.1\nHost: io.adafruit.com\n" \
                                      "X-AIO-Key: b19057d0daee4a4db05b4c0c1ed9166d\nContent-Type: application/json\nContent-Length: ";
  for (;;) {
    xSemaphoreTake(LiveSH, portMAX_DELAY); //wait until SensorTask finish reading sensors
    if (now != currentTime)
      for (;;) {
        if (!client.connected()) while (!client.connect("io.adafruit.com", 80)) Serial.println("Adafruit Connection Failed");
        Serial.println("Uploading to Adafruit");
        if (now != currentTime) { //update the request when there is new sensor data
          xSemaphoreTake(sensorsSH, portMAX_DELAY);
          payload = String("{\"feeds\":[{\"key\":\"cond\",\"value\":\"") + conductivity
                    + "\"},{\"key\":\"ph\",\"value\":\"" + pH
                    + "\"},{\"key\":\"temp\",\"value\":\"" + temperature + "\"}]}";
          xSemaphoreGive(sensorsSH);
          request = adafruit + payload.length() + "\n\n" + payload;
          now = currentTime;
        }
        start = millis();
        client.print(request);
        while (!client.available()) {
          if (millis() - start > 5000 || now != currentTime) break; //during 5 secs, if there is new sensor data, break immediately
          delay(10);
        }
        if (client.available()) {
          if (client.findUntil("200", "\n")) {
            Serial.println("Adafruit Upload success");
          } else {
            delay(2000);
          }
          while (client.available()) client.read();
          break;
        }
        client.stop();
      }
  }
}

void TimeTask(void* param) {
  vTaskSuspend(NULL);
  WiFiUDP Udp;
  unsigned long start;
  Udp.begin(8888);
  byte packet[48], t = 0;
  for (;;) {
    xSemaphoreTake(TimeSH, portMAX_DELAY);
    if (currentTime - syncTime > 5184000) { //if current time differ 2 months from last sync time
      for (;;) {
        if (WiFi.status() == WL_DISCONNECTED) {
          Serial.println("TimeTask Suspended");
          vTaskSuspend(NULL); //suspend TimeTask itself manually outside WiFiEvent when wifi disconnected
        }
        Serial.println("Time Syncing");
        packet[0] = 0b11100011; packet[1] = 0; packet[2] = 6; packet[3] = 0xEC;
        packet[12] = 49; packet[13] = 0x4E; packet[14] = 49; packet[15] = 52;
        if (Udp.beginPacket("2.asia.pool.ntp.org", 123) && Udp.write(packet, 48) && Udp.endPacket()) {
          t = 0;
          start = millis();
          while (t < 48) {
            t = Udp.parsePacket();
            if (millis() - start > 1500) break;
            delay(10);
          }
          if (t >= 48) {
            syncMillis = millis(); //save current millis when time is updated
            Udp.read(packet, 48);  // read packet into the buffer
            syncTime = packet[40] << 24 | packet[41] << 16 | packet[42] << 8 | packet[43]; //time is 32 bits (4 bytes)
            syncTime = syncTime - 2208988800UL + 7 * 3600; // ntp time is from 1900, utc time is from 1970, GMT+7
            getTimeOk = true;
            tm time_h;
            gmtime_r((time_t*)&syncTime, &time_h);
            DS3231_set(ts {time_h.tm_sec, time_h.tm_min, time_h.tm_hour, time_h.tm_mday, time_h.tm_mon + 1, time_h.tm_year + 1900}); //store time to DS module
            preferences.putUInt("sync_time", syncTime); //store time to Esp32
            Serial.printf("%d-%02d-%02d %02d:%02d:%02d\n", time_h.tm_year + 1900, time_h.tm_mon + 1, time_h.tm_mday, time_h.tm_hour, time_h.tm_min, time_h.tm_sec);
            break;
          }
        }
      }
    }
  }
}

void CloudTask(void* param) {
  vTaskSuspend(NULL);
  WiFiClientSecure scriptClient;
  String gas = "POST https://script.google.com/macros/s/AKfycbxKqHFAW6NwkM9-kg4gvJYG6OnegDolzofv_q3RjNF_x-gDcXIf/exec HTTP/1.1\nContent-Length: ";
  String drive = "POST https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart HTTP/1.1\nAuthorization: Bearer ";
  String payload, request, token = "abc";
  unsigned long uploadTime, now = 0, start;
  if (!SD.exists("/esp32")) SD.mkdir("/esp32"); //store files in sdcard/esp32 folder
  uploadTime = SD.open("/time").parseInt(); //store lastest line upload time in sdcard/time file
  File file, esp32 = SD.open("/esp32"), timeFile;
  for (;;) {
    xSemaphoreTake(CloudSH, portMAX_DELAY);
    if (uploadTime != currentTime) {
      while (!uploadStringOk) {
        if (now != currentTime) { //add new sensor data to request after timer period if connection fail
          if (count <  5 ) { //250 //maximum 250 lines of sensor data in request due to Google App Script long processing time
            now = currentTime;
            xSemaphoreTake(sensorsStringSH, portMAX_DELAY);
            payload += sensorsString;
            xSemaphoreGive(sensorsStringSH);
            request = gas + payload.length() + "\n\n" + payload;
            if (count == 254 || uploadStringOk) { //if this task is resumed after wifi disconnected or SensorTask create new file, break out of while loop to prevent missing data or adding new data to old file
              count = 0;
              payload = "";
              break;
            }
            count++;
          }
        }
        if (scriptClient.connected() || scriptClient.connect("script.google.com", 443)) { //while (!uploadStringOk) replace while (!scriptClient.connect)
          Serial.println("Uploading string to GAS");
          scriptClient.print(request);
          start = millis();
          while (!scriptClient.available()) {
            if (millis() - start > 5000) break;
            delay(10);
          }
          if (scriptClient.available()) {
            Serial.println("GAS string Upload success");
            timeFile = SD.open("/time", FILE_WRITE);
            timeFile.print(now); //last line
            timeFile.close();
            count = 0;
            payload = "";
            uploadTime = now;
            while (scriptClient.available()) scriptClient.read();
            break;
          }
          scriptClient.stop();
        }
      }

      uploadStringOk = false;
      if (currentTime != uploadTime) {
        Serial.println("currentTime != uploadTime");
        if (uploadTime != 0) { //uploadTime = 0 means never upload to Cloud
          tm time_h;
          char uploadFileName[20]; //the file that contains uploadTime
          time_t x = uploadTime - uploadTime % 120;
          gmtime_r(&x, &time_h);
          sprintf(uploadFileName, "/esp32/%d%02d%02d%02d%02d", time_h.tm_year + 1900, time_h.tm_mon + 1, time_h.tm_mday, time_h.tm_hour, time_h.tm_min);
          while (String(file.name()) != String(uploadFileName)) { //iterate through esp32 folder until find the lastest uploaded file
            file = esp32.openNextFile();
            Serial.println("a");
          }
          file.find(String(uploadTime).c_str()); file.find("\n"); //go to the end of the line that contains uploadTime, start processing from here
        }
        int i, length;
        bool isLastestFile;
        uint32_t readPosition;
        for (;;) {
          xSemaphoreTake(fileNameSH, portMAX_DELAY);
          isLastestFile = String(file.name()) == String(fileName);
          xSemaphoreGive(fileNameSH);
          while (file.available()) { //upload until end of current file
            readPosition = file.position();
            i = 0;
            while (i < 5 ) { //250 //maximum 250 lines in request
              if (file.find("\n")) i++;
              if (!file.available()) break;
            }
            length = file.position() - readPosition;
            uint8_t bf[length];
            file.seek(readPosition);
            file.read(bf, length);
            request = gas + length + "\n\n";
            for (;;) {
              if (!scriptClient.connected()) while (!scriptClient.connect("script.google.com", 443)) Serial.println("script Connection failed");
              Serial.println("Uploading lines to GAS");
              scriptClient.print(request);
              scriptClient.write(bf, length);
              start = millis();
              while (!scriptClient.available()) {
                if (millis() - start > 5000) break;
                delay(10);
              }
              if (scriptClient.available()) {
                Serial.println("GAS lines Upload success");
                if (i == 1) {
                  file.seek(readPosition);
                } else {
                  file.seek(file.position() - 33);
                  file.find("\n");
                }
                uploadTime = file.parseInt();
                timeFile = SD.open("/time", FILE_WRITE);
                timeFile.print(uploadTime); //last line
                timeFile.close();
                file.find("\n");
                while (scriptClient.available()) scriptClient.read();
                break;
              }
              scriptClient.stop();
            }
          }
          if (isLastestFile) {
            Serial.println("lastest File");
            if (uploadTime == currentTime) break; //stop uploading from sdcard
            else {
              readPosition = file.position();
              file = SD.open(file.name()); //open current file again and read from last upload position
              file.seek(readPosition);
            }
          } else {
            Serial.println("not Lastest File");
            file = esp32.openNextFile();
            WiFiClientSecure driveClient;
            uint8_t bf[10000];
            xSemaphoreTake(fileNameSH, portMAX_DELAY);
            while (String(file.name()) != String(fileName)) {
              xSemaphoreGive(fileNameSH);
              request = drive + token + "\nContent-Type: multipart/related; boundary=k8c\nContent-Length: " + (168 + file.size()) +
                        "\n\n--k8c\nContent-Type: application/json; charset=UTF-8\n\n{\"name\":\"" + String(file.name()).substring(7) +
                        "\",\"mimeType\":\"application/vnd.google-apps.document\"}\n\n--k8c\nContent-Type: text/plain\n\n";
              for (;;) {
                if (!driveClient.connected()) while (!driveClient.connect("googleapis.com", 443)) Serial.println("drive Connection failed");
                Serial.print("Uploading to Drive file: "); Serial.println(file.name());
                driveClient.print(request);
                while (file.available()) driveClient.write(bf, file.read(bf, 10000));
                driveClient.print("\n--k8c--");
                start = millis();
                while (!driveClient.available()) {
                  if (millis() - start > 10000) break;
                  delay(10);
                }
                if (driveClient.available()) {
                  if (driveClient.findUntil("200", "\n")) {
                    Serial.println("Drive Upload success");
                    if (file.size() > 33) {
                      file.seek(-33, SeekEnd);
                      file.find("\n");
                    } else file.seek(0);
                    timeFile = SD.open("/time", FILE_WRITE);
                    timeFile.print(file.readStringUntil(',')); //last line
                    timeFile.close();
                    while (driveClient.available()) driveClient.read();
                    break;
                  } else { //upload request error, assume only because access token expired
                    driveClient.stop();
                    for (;;) {
                      while (!driveClient.connect("googleapis.com", 443)) Serial.println("auth Connection failed");
                      driveClient.print("POST /oauth2/v4/token?client_id=463875113005-icovngqrabn2hass5tug5ik5m436ks2k.apps.googleusercontent.com&" \
                                        "client_secret=8PWn96NTst2-rbkaXToWoi6F&refresh_token=1/7C-dMwDk771wT5lads8os4_mziPZspcIU6ndw_ZJpi4&grant_type=refresh_token HTTP/1.1\n" \
                                        "Host: www.googleapis.com\n" \
                                        "Content-Length: 0\n\n");
                      start = millis();
                      while (!driveClient.available()) {
                        if (millis() - start > 5000) {
                          Serial.println("auth Timeout");
                          break;
                        }
                        delay(10);
                      }
                      if (driveClient.available()) {
                        driveClient.find("s_token\": \""); // access_token
                        token = driveClient.readStringUntil('"');
                        Serial.print("token: "); Serial.println(token);
                        request = drive + token + "\nContent-Type: multipart/related; boundary=k8c\nContent-Length: " + (168 + file.size()) +
                                  "\n\n--k8c\nContent-Type: application/json; charset=UTF-8\n\n{\"name\":\"" + String(file.name()).substring(7) +
                                  "\",\"mimeType\":\"application/vnd.google-apps.document\"}\n\n--k8c\nContent-Type: text/plain\n\n";
                        break;
                      }
                      driveClient.stop();
                    }
                  }
                }
                driveClient.stop();
                file.seek(0); //upload file fail -> read and upload file again
              }
              file = esp32.openNextFile(); //open next file until file.name match lastest file name (isLastestFile = true)
              xSemaphoreTake(fileNameSH, portMAX_DELAY);
            }
            xSemaphoreGive(fileNameSH);
          }
        }
      }
    }
  }
}
