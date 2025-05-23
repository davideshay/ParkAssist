#include <Arduino.h>
#include <parkassist.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
#include <wificodes.h>
#include <HCSR04.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <leds.h>
#include <camera.h>
#include <parkassist.h>
#include <ota.h>
#include <log.h>
#include <FastLED.h>
#include <PrettyOTA.h>
#include <SimpleKalmanFilter.h>
#include "driver/temp_sensor.h"

// 
// PIN LAYOUT -- all defined in parkassist.h
// 
// Camera uses pins 4,5,6,7,15,16,17,18,8,9,10,11,12,13
//    (all of one side of the board except for 3 JTAG and 46 LOG
//
// #define TEMP_SENSOR_BUS 14.   (OneWire DS18B20)
// #define DIST_SENSOR_TRIGGER 19. (HC-SR04)
// #define DIST_SENSOR_ECHO 20. (HC-SR04)
// #define IR_BREAK_SENSOR 21
// #define LED_PANEL_PIN 47. (WS2812 panel)

// structureof carLogo - 1 blank bit, followed by 3 bits (color 0-7) each 5x to represent first 5 pixels. 
// Cover 60 pixels -- 6 rows x 10 columns. Go row-by-row in order. So first int_16 is first 5 pixels.
// Second int_16 is 5 remaining pixels in first row, then on to 2nd row.

carInfoStruct defaultCar = 
  { .targetFrontDistanceCm = 79, .maxFrontDistanceCm = 50, .lengthOffsetCm = 0, .sensorDistanceFromFrontCm = 550,
     .carLogo = 
    {
      0b0100100100000000,
      0b0100000100100100,
      0b0100000100000100,
      0b0100000100000000,
      0b0100100100000000,
      0b0100000100000000,
      0b0100100000000000,
      0b0100000100100100,
      0b0100000100000000,
      0b0100000000000100,
      0b0100000100000000,
      0b0100000100100100
    },
    .logoColors = {
      CRGB::Black,
      CRGB::White,
      CRGB::Red,
      CRGB::Green,
      CRGB::Blue,
      CRGB::Yellow,
      CRGB::Orange,
      CRGB::Purple }
  };

carInfoStruct currentCar;

stateOpts curState = BASELINE;

int secsToReset = 60;
int64_t camera_checking_millis = 0;
int64_t maxCameraCheckMillis = 1000;
int64_t timer_started_millis = 0;

int64_t dist_check_millis = 0;
const int64_t time_between_dist_checks_millis = 60;
int64_t temp_check_millis = 0;
const int64_t time_between_temp_checks_millis = 60000;
SimpleKalmanFilter kalmanFilter(75,75,3);

AsyncWebServer serverOTA(80);
PrettyOTA OTAUpdates;

UltraSonicDistanceSensor distanceSensor(DIST_SENSOR_TRIGGER, DIST_SENSOR_ECHO);  // Initialize sensor that uses digital pins 42 and 41

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(TEMP_SENSOR_BUS);

// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature tempSensors(&oneWire);

float currentTemp;
double currentDistance;
const double maxDistanceDeltaOK = 50;
distanceEvaluation currentDistanceEvaluation;
double carFirstDetectedDistanceFromFront;
boolean carFirstDetected = false;
double carFirstClearedSensorDistanceFromFront;
boolean carFirstClearedSensor = false;
double realDistanceDetected = false;
boolean carDetected;
boolean useMetric = false;

boolean demoMode = false;
double demoDistance;
boolean demoIRBREAK = true;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(IR_BREAK_SENSOR, INPUT);
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");
 
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  initLogging();
 
  serverOTA.begin();
  Serial.println("OTA HTTP server started");

  serverOTA.begin();
  OTAUpdates.Begin(&serverOTA);
  OTAUpdates.OverwriteAppVersion("1.0.0");
  PRETTY_OTA_SET_CURRENT_BUILD_TIME_AND_DATE();
  OTAUpdates.OnStart(onOTAStart);
  OTAUpdates.OnProgress(onOTAProgress);
  OTAUpdates.OnEnd(onOTAEnd);

  tempSensors.begin();
  initCamera();
  initLEDs();
  
  //TODO -- DELETE LATER
  currentCar = defaultCar;
}

distanceEvaluation evaluateDistance() {
  // TODO -- does not deal with lengthOffsets for other cars
  distanceEvaluation distEval = {
    .colorRGB = CRGB::Blue,
    .colorCode = BLUE,
    .colorOffset = 0,
    .displayInfinity = false,
    .inchesToTarget = 0,
    .cmToTarget = 0,
    .displayDistance = 0
  };
  double smoothedDistance = currentDistance;
  if (currentDistance == -1 || currentDistance < 0) {
    smoothedDistance = currentCar.sensorDistanceFromFrontCm;
  };
  if (carDetected) {
    distEval.colorRGB = CRGB::Yellow;
    distEval.colorCode = YELLOW;
    // car is at 120cm, first detected car at 200cm, target = 90 --> 110 cm range from first detected to target
    // how far inside = 200-120 = 80cm, so 80/110 = 45% of range, offset should be 2
    distEval.colorOffset = constrain(((carFirstDetectedDistanceFromFront - smoothedDistance) / (carFirstDetectedDistanceFromFront - currentCar.targetFrontDistanceCm))*yellowBoxMaxOffset,0,yellowBoxMaxOffset);
  } else {
    if (smoothedDistance < currentCar.targetFrontDistanceCm) {
      distEval.colorRGB = CRGB::Red;
      distEval.colorCode = RED;
      // car is at 80cm, target=90, max=60 --> 30 cm range from target to max
      // how far inside that range = 90-80 = 10cm, so using 30% of range, offset should be 1
      distEval.colorOffset = constrain((((currentCar.targetFrontDistanceCm - smoothedDistance) / (currentCar.targetFrontDistanceCm - currentCar.maxFrontDistanceCm))*redBoxMaxOffset),0,redBoxMaxOffset);
    } else {
      // car cleared sensor at 100cm, car is now at 95cm, target is 90 --> 10cm range from cleared->target
      // how far inside that range = 100-95 = 5cm/10cm --> offset = 2
      distEval.colorOffset = constrain(((carFirstClearedSensorDistanceFromFront - smoothedDistance) / (carFirstClearedSensorDistanceFromFront - currentCar.targetFrontDistanceCm)*greenBoxMaxOffset),0,greenBoxMaxOffset);
      distEval.colorRGB = CRGB::Green;
      distEval.colorCode = GREEN;
    }  
  }
  distEval.cmToTarget = smoothedDistance - currentCar.targetFrontDistanceCm;
  distEval.inchesToTarget = (distEval.cmToTarget / 2.54);
  if (useMetric) {
    if (distEval.cmToTarget > 99 || distEval.cmToTarget < -99) {
      distEval.displayInfinity = true;}
    else {
      distEval.displayDistance = distEval.cmToTarget;
    }
  }
  else {
    if (distEval.inchesToTarget > 99 || distEval.inchesToTarget < -99) {
      distEval.displayInfinity = true;}
    else {
      distEval.displayDistance = distEval.inchesToTarget;
    }
  }
  return distEval;
}

void getTemperature() {
  tempSensors.requestTemperatures();
  if (tempSensors.getTempCByIndex(0) > -127) {
    currentTemp = tempSensors.getTempCByIndex(0);
  } else {
    logData("No reading available from temperature sensor, setting to 20C/68F",true);
    currentTemp = 20;
  }
}

void getIRBreak() {
  bool irBreak;
  if (demoMode) {irBreak = demoIRBREAK;} else {irBreak = digitalRead(IR_BREAK_SENSOR) == LOW;}
  if (irBreak) {
    carDetected = true;
    if (!carFirstDetected) {
      carFirstDetected = true;
      carFirstDetectedDistanceFromFront = currentDistance;
      String msg = "First Detected Car at distance: ";
      msg += carFirstDetectedDistanceFromFront;
      logData(msg,true);
    }
  } else {
    if (carDetected) {
      carFirstClearedSensor = true;
      carFirstClearedSensorDistanceFromFront = currentDistance;
      String msg = "First Detected Car Cleared Sensor at distance: ";
      msg += carFirstClearedSensorDistanceFromFront;
      logData(msg,true);
    }
    carDetected = false;
  }
}

double getSensorDistance() {
  return distanceSensor.measureDistanceCm(currentTemp);
}

void getCurrentData() {
  if (esp_millis() - dist_check_millis < time_between_dist_checks_millis) {return;}
  if (demoMode) {
    currentDistance = demoDistance;
  } else {
    double origDistance = getSensorDistance();
//    String m = "ood=";
//    m += origDistance;
//    logData(m,false);
    double corrDistance;
    if ((corrDistance == -1 || corrDistance < 0)) {
      if (realDistanceDetected) {
      // At some point, I had already detected a real distance, but now I don't
      // This is either bad sensor data (99% of the time) or could be car backing out and can no longer be seen
      return;
      } else {
      // Never had a real distance detected , default value to the max distance in the garage (distance to door)
        corrDistance = currentCar.sensorDistanceFromFrontCm;
      }
    } else {
      // A "real" distance has come in from the sensor. If it deviates by more than 50cm from the current distance
      // then throw it out if this is not the first reading
      if (realDistanceDetected) {
        // if (abs(currentDistance - origDistance) > 50) {return;}
      } else {
        // This is the first real distance to be detected -- accept the value without deviation check
        realDistanceDetected = true;
      }
      corrDistance = origDistance;
    }
    
    float estimatedDistance = kalmanFilter.updateEstimate(corrDistance);
    logDetailData(origDistance,corrDistance,estimatedDistance);
    currentDistance = estimatedDistance;
  }
  getIRBreak();
  currentDistanceEvaluation = evaluateDistance();
  dist_check_millis = esp_millis();
};

void displayCurrentData() {
  logCurrentData();
  FastLED.clear();
  drawDistance(currentDistance,useMetric,currentDistanceEvaluation,currentCar);
  drawDistanceWord(useMetric,currentDistanceEvaluation);
  drawCarLogo(currentCar);
  drawPictureGuide(currentDistanceEvaluation);
  FastLED.show();
}

void resetBaseline() {
  FastLED.clear();
  FastLED.show();
  curState = BASELINE;
  carFirstDetected = false;
  carFirstDetectedDistanceFromFront = 0;
  carFirstClearedSensor = false;
  carFirstClearedSensorDistanceFromFront = 0;
  realDistanceDetected = false;
  currentDistance = 0;
  closeLogFile();
}

void loop() {
    WebSerial.loop();
    if (esp_millis() - temp_check_millis > time_between_temp_checks_millis) {
      getTemperature();
      temp_check_millis = esp_millis();
    }

    switch (curState) {
      case BASELINE:
        if (digitalRead(IR_BREAK_SENSOR) == LOW) {
          logData("IR Sensor Broken from default. Car Present",true);
          timer_started_millis = esp_millis();
          carDetected = true;
          curState = CAR_PRESENT;
          openLogFileAppend();
        }
        break;
      case CAR_PRESENT:
        curState = DETECTING_CAR_TYPE;
        camera_checking_millis = esp_millis();
        logData("Now Detecting Car Type...",true);
        break;
      case DETECTING_CAR_TYPE:
        if (esp_millis() - camera_checking_millis > maxCameraCheckMillis) {
          logData("Car Detection Timeout, set to default",true);
          currentCar = defaultCar;
          curState = CAR_TYPE_DETECTED;
        }
        break;
      case CAR_TYPE_DETECTED:
        curState = SHOWING_DATA;
        logData("Car Detected. Showing Data...",true);
        timer_started_millis = esp_millis();
        break;
      case SHOWING_DATA:
        getCurrentData();
        displayCurrentData();
        if (((esp_millis() - timer_started_millis) > (unsigned long)(secsToReset * 1000))  && !carDetected) {
            logData("Timer expired and no car Detected",true);
            curState = TIMER_EXPIRED;
        }
        break;
      case TIMER_EXPIRED:
        logData("Timer expired, turning off display and resetting to baseline",true);
        resetBaseline();        
    }

}

