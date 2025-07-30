#define MS_TO_TICKS(ms) ((ms) / portTICK_PERIOD_MS)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include "FS.h"
#include "SD.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <addons/TokenHelper.h>

static const uint8_t SCREEN_ADDRESS = 0x3C;
static const uint8_t SENSOR_ADDRESS = 0x76;

static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const int8_t OLED_RESET = -1;

const char* WIFI_SSID = "XD";
const char* W IFI_PASSWORD = "11081975";

const char* WEB_API_KEY = "AIzaSyCkosRZUHBpq2QKOnVbxIqGwuZvKwB51oc";
const char* DATABASE_URL = "https://freertos-5377b-default-rtdb.asia-southeast1.firebasedatabase.app/";
const char* USER_EMAIL = "waleed7x.spam@gmail.com";
const char* USER_PASS = "waleed.spam200657";
const char* FIREBASE_PATH = "/devices/weather_station_01";

Adafruit_BMP280 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

static const int BAUD_RATE = 115200;
static const uint8_t MY_SDA = 21;
static const uint8_t MY_SCL = 22;
static const uint8_t LED = LED_BUILTIN;

static const uint8_t DISPLAY_CURSOR_X = 0;
static const uint8_t DISPLAY_CURSOR_Y = 0;
static const uint8_t DISPLAY_TEXT_SIZE = 2;
static const uint8_t DISPLAY_TEXT_COLOR = SSD1306_WHITE;

static TaskHandle_t readSensor_h = NULL;
static TaskHandle_t displayData_h = NULL;
static TaskHandle_t sdCardLogger_h = NULL;
static TaskHandle_t readSerial_h = NULL;
static TaskHandle_t firebaseUpload_h = NULL;

static SemaphoreHandle_t sensor_mutex;
static SemaphoreHandle_t i2c_mutex;
static SemaphoreHandle_t spi_mutex;

static const int SENSOR_MUTEX_WAIT_MS = 10;
static const int I2C_MUTEX_WAIT_MS = 100;
static const int SPI_MUTEX_WAIT_MS = 100;

static const int WIFI_CONNECT_INTERVAL_MS = 500;
static const int SENSOR_READ_INTERVAL_MS = 1000;
static const int DISPLAY_UPDATE_INTERVAL_MS = 1000;
static const int SDCARD_SAMPLE_INTERVAL_MS = 1000; 
static const int FIREBASE_SAMPLE_INTERVAL_MS = 1000; 
static const int NO_ERROR_LED_INTERVAL_MS = 2500;
static const int HW_ERROR_LED_INTERVAL_MS = 500;
static const int HARDWARE_CHECK_INTERVAL_MS = 5000;

static const uint8_t MAX_SDCARD_SAMPLES = 30;
static const uint8_t MAX_FIREBASE_SAMPLES = 60;

static const uint8_t SERIAL_BUFFER_SIZE = 20;
static const uint8_t SD_CARD_FOLDER_PATH_SIZE = 20;
static const uint8_t SD_CARD_FILE_PATH_SIZE = 40;
static const uint8_t SD_CARD_TIME_SIZE = 10;

typedef struct {
  float temperature;
  float pressure;
} SensorData_t;

static SensorData_t sensor_data;

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void tokenStatusCallback(TokenInfo info) {
  if (info.status == token_status_ready) {
    Serial.println("Firebase: Token is ready, authentication successful.");
  } 
  else {
    Serial.printf("Firebase Task: Token status: %s\n", getAuthTokenStatus(info).c_str());
    Serial.printf("Firebase Task: Token error: %s\n", info.error.message.c_str());
  }
}

float toFahrenheit(float celsius) {
  return celsius * 9.0 / 5.0 + 32.0;
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

float calculateAverageTemp(const SensorData_t* sensor_data, uint8_t data_count) {
  if (data_count == 0) {
    return 0.0;
  }

  float avg_temp = 0.0;
  for (int i = 0; i < data_count; i++) {
    avg_temp += sensor_data[i].temperature;
  }
  return avg_temp / data_count;
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

float calculateAveragePressure(const SensorData_t* sensor_data, uint8_t data_count) {
  if (data_count == 0) {
    return 0.0;
  }

  float avg_pres = 0.0;
  for (int i = 0; i < data_count; i++) {
    avg_pres += sensor_data[i].pressure;
  }
  return avg_pres / data_count;
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

const char* getMonthName(int month) {
  const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (month >= 0 && month < 12) {
    return months[month];
  }
  return "Unknown";
}

bool checkHardware() {
  bool hardware_status = true;

  if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS) == pdTRUE)) {
    if (!bmp.begin(SENSOR_ADDRESS)) {
      Serial.println("System Monitor: BMP280 not found.");
      hardware_status = false;
    }
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
      Serial.println("System Monitor: SSD1306 not found.");
      hardware_status = false;
    }
    xSemaphoreGive(i2c_mutex);
  }

  return hardware_status;
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

bool isValidSerialInput(char* input) {

}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void readSensor(void* p) {
  SensorData_t fresh_sensor_data;

  while(1) {
    if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS)) == pdTRUE) {
      fresh_sensor_data.temperature = bmp.readTemperature();
      fresh_sensor_data.pressure = bmp.readPressure() / 100.0;

      xSemaphoreGive(i2c_mutex);
    }

    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      sensor_data.temperature = fresh_sensor_data.temperature;
      sensor_data.pressure = fresh_sensor_data.pressure;
      
      xSemaphoreGive(sensor_mutex);
    }

    vTaskDelay(MS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void displayData(void* p) {
  SensorData_t local_sensor_data;

  while(1) {
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      local_sensor_data = sensor_data;

      xSemaphoreGive(sensor_mutex);
    }

    if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS)) == pdTRUE) {
      display.clearDisplay();
      display.setTextColor(DISPLAY_TEXT_COLOR);
      display.setTextSize(DISPLAY_TEXT_SIZE);
      display.setCursor(DISPLAY_CURSOR_X, DISPLAY_CURSOR_Y);

      display.println("BMP 280 Data:\n");

      display.print(local_sensor_data.temperature); 
      display.println(" C");

      display.print(toFahrenheit(local_sensor_data.temperature)); 
      display.println(" F");

      display.print(local_sensor_data.pressure); 
      display.println(" hPa");

      display.display();

      xSemaphoreGive(i2c_mutex);
    }

    vTaskDelay(MS_TO_TICKS(DISPLAY_UPDATE_INTERVAL_MS));
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void sdCardLogger(void* p) {
  SensorData_t local_sensor_data[MAX_SDCARD_SAMPLES];
  uint8_t sensor_data_count = 0;

  SensorData_t avg_sensor_data;

  while(1) {
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      local_sensor_data[sensor_data_count++] = sensor_data;

      xSemaphoreGive(sensor_mutex);
    }

    if (sensor_data_count == MAX_SDCARD_SAMPLES) {
      avg_sensor_data.temperature = calculateAverageTemp(local_sensor_data, sensor_data_count);
      avg_sensor_data.pressure = calculateAveragePressure(local_sensor_data, sensor_data_count);

      struct tm time_info;
      if (!getLocalTime(&time_info)) {
        Serial.println("SD Card Task: Failed to get time. Skipping log.");
        continue;
      }
      
      char folder_path[SD_CARD_FOLDER_PATH_SIZE];
      snprintf(folder_path, sizeof(folder_path), "/%s_%d", getMonthName(time_info.tm_mon), time_info.tm_year + 1900);

      char file_path[SD_CARD_FILE_PATH_SIZE];
      snprintf(file_path, sizeof(file_path), "%s/%d_%s_%d.csv", folder_path, time_info.tm_mday, getMonthName(time_info.tm_mon), time_info.tm_year + 1900);

      if (xSemaphoreTake(spi_mutex, MS_TO_TICKS(SPI_MUTEX_WAIT_MS)) == pdTRUE) {
        if (!SD.exists(folder_path)) {
          if (!SD.mkdir(folder_path)) {
            Serial.println("SD Card Task: Folder creation failed. Skipping log.");
            xSemaphoreGive(spi_mutex);
            continue;
          }
        }

        if (!SD.exists(file_path)) {
          File file = SD.open(file_path, FILE_WRITE);
          if (file) {
            file.println("Time,Temperature_C,Temperature_F,Pressure_hPa");
            file.close();
          }
          else {
            Serial.println("SD Card Task: File creation failed. Skipping log.");
            xSemaphoreGive(spi_mutex);
            continue;
          }
        }

        File file = SD.open(file_path, FILE_APPEND);
        if (file) {
          char time[SD_CARD_TIME_SIZE];
          strftime(time, sizeof(time), "%H:%M:%S", &time_info);

          file.printf("%s,%.2f,%.2f,%.2f\n", time, avg_sensor_data.temperature, toFahrenheit(avg_sensor_data.temperature), avg_sensor_data.pressure);
          file.close();
        }
        else {
          Serial.println("SD Card Task: File write failed. Skipping log.");
          xSemaphoreGive(spi_mutex);
          continue;
        }

        xSemaphoreGive(spi_mutex);
      }

      sensor_data_count = 0;
    }

    vTaskDelay(MS_TO_TICKS(SDCARD_SAMPLE_INTERVAL_MS));
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void firebaseUpload(void* p) {
  SensorData_t local_sensor_data[MAX_FIREBASE_SAMPLES];
  uint8_t sensor_data_count = 0;

  SensorData_t avg_sensor_data;

  while(1) {
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      local_sensor_data[sensor_data_count++] = sensor_data;

      xSemaphoreGive(sensor_mutex);
    }

    if (sensor_data_count == MAX_FIREBASE_SAMPLES) {
      if (Firebase.ready()) {
        avg_sensor_data.temperature = calculateAverageTemp(local_sensor_data, sensor_data_count);
        avg_sensor_data.pressure = calculateAveragePressure(local_sensor_data, sensor_data_count);

        Firebase.RTDB.setFloatAsync(&fbdo, FIREBASE_PATH + "/temperature_c", avg_sensor_data.temperature);
        Firebase.RTDB.setFloatAsync(&fbdo, FIREBASE_PATH + "/temperature_f", toFahrenheit(avg_sensor_data.temperature));
        Firebase.RTDB.setFloatAsync(&fbdo, FIREBASE_PATH + "/pressure_hpa", avg_sensor_data.pressure);
        Firebase.RTDB.setTimestampAsync(&fbdo, FIREBASE_PATH + "/last_updated_timestamp");
      }
      else {
        Serial.println("Firebase Task: Firebase not ready. Skipping upload.");
      }

      sensor_data_count = 0;
    }

    vTaskDelay(MS_TO_TICKS(FIREBASE_SAMPLE_INTERVAL_MS));
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void readSerial(void* p) {
  char buffer[SERIAL_BUFFER_SIZE];
  char ch;
  uint8_t index = 0;

  memset(buffer, 0, SERIAL_BUFFER_SIZE); 

  while(1) {
    if (Serial.available()) {
      ch = Serial.read();

      if (ch == '\n') {
        buffer[index] = '\0';

        if (isValidSerialInput(buffer)) {

        }

        memset(buffer, 0, SERIAL_BUFFER_SIZE);
        index = 0;
      }
      else {
        if (index < SERIAL_BUFFER_SIZE - 1) {
          buffer[index++] = ch;
        }
      }
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void systemMonitor(void* p) {
  typedef enum {HARDWARE_INIT, HARDWARE_ERROR, RUNNING} SystemState_t;
  static SystemState_t system_state = HARDWARE_INIT;

  TickType_t hardware_check_start_time = xTaskGetTickCount();

  bool tasks_running = false;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("System Monitor: Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(MS_TO_TICKS(WIFI_CONNECT_INTERVAL_MS));
  }
  Serial.println();
  Serial.println("System Monitor: Connected.");

  config.api_key = WEB_API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASS;
  config.database_url = DATABASE_URL;

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWifi(true);

  while(1) {
    switch (system_state) {
      case HARDWARE_INIT:
        Serial.println("System Monitor: Checking hardware.");
        if (checkHardware()) {
          if (!tasks_running) {
            Serial.println("Serial Monitor: Hardware OK. Initializing system.");

            xTaskCreatePinnedToCore(readSensor, "Read Sensor", 2048, NULL, 4, &readSensor_h, 0);
            xTaskCreatePinnedToCore(displayData, "Display Data", 2048, NULL, 3, &displayData_h, 0);
            xTaskCreatePinnedToCore(sdCardLogger, "Data to SD Card", 2048, NULL, 2, &sdCardLogger_h, 0);

            xTaskCreatePinnedToCore(readSerial, "Read Serial", 2048, NULL, 3, &readSerial_h, 1);
            xTaskCreatePinnedToCore(firebaseUpload, "Data to Firebase", 8192, NULL, 2, &firebaseUpload_h, 1);

            tasks_running = true;
          }
          else {
            Serial.println("System Monitor: Hardware recovery successful. Resuming tasks.");

            vTaskResume(readSensor_h);
            vTaskResume(displayData_h);
            vTaskResume(sdCardLogger_h);
            vTaskResume(readSerial_h);
            vTaskResume(firebaseUpload_h);
          }

          system_state = RUNNING;
          hardware_check_start_time = xTaskGetTickCount();
        }
        else {
          system_state = HARDWARE_ERROR;
          hardware_check_start_time = xTaskGetTickCount();
        }
        break;

      case HARDWARE_ERROR:
        digitalWrite(LED, HIGH);
        vTaskDelay(MS_TO_TICKS(100));
        digitalWrite(LED, LOW);
        vTaskDelay(MS_TO_TICKS(HW_ERROR_LED_INTERVAL_MS));

        if (xTaskGetTickCount() - hardware_check_start_time >= MS_TO_TICKS(HARDWARE_CHECK_INTERVAL_MS)) {
          Serial.println("System Monitor: Checking hardware.");
          if (checkHardware()) {
            system_state = HARDWARE_INIT;
          }
          else {
            hardware_check_start_time = xTaskGetTickCount();
          }
        }
        break;

      case RUNNING:
        digitalWrite(LED, HIGH);
        vTaskDelay(MS_TO_TICKS(100));
        digitalWrite(LED, LOW);
        vTaskDelay(MS_TO_TICKS(NO_ERROR_LED_INTERVAL_MS));

        if (xTaskGetTickCount() - hardware_check_start_time >= MS_TO_TICKS(HARDWARE_CHECK_INTERVAL_MS)) {
          if (!checkHardware()) {
            vTaskSuspend(readSensor_h);
            vTaskSuspend(displayData_h);
            vTaskSuspend(sdCardLogger_h);
            vTaskSuspend(readSerial_h);
            vTaskSuspend(firebaseUpload_h);

            system_state = HARDWARE_ERROR;
          }

          hardware_check_start_time = xTaskGetTickCount();
        }
        break;
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////

void setup() {
  Serial.begin(BAUD_RATE);
  Wire.begin(MY_SDA, MY_SCL);
  pinMode(LED, OUTPUT);

  vTaskDelay(MS_TO_TICKS(1000));

  sensor_mutex = xSemaphoreCreateMutex();
  i2c_mutex = xSemaphoreCreateMutex();
  spi_mutex = xSemaphoreCreateMutex();  

  xTaskCreatePinnedToCore(systemMonitor, "System Monitor", 4096, NULL, 5, NULL, 0);
}

void loop() {
  Firebase.loop();
}