// Note: You might see a "Wire.cpp" I2C error in the serial monitor
// if sensor or dipslay is disconnected while the system is running.
// This is due to the detection latency between the hardware check (5 seconds) 
// and the sensor read (1 second). 
// This can easily be solved by setting "HARDWARE_CHECK_INTERVAL_MS" to *1000*
// But at the cost of performance becasue the hardware check will run every second.
// The error is completely harmless and can be ignored.
// That is why I have set the hardware check interval to 5 seconds.

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

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

// Macro to convert milliseconds to FreeRTOS ticks.
// Used this to make code more generic and usable
// on other devices with different tick rates.
#define MS_TO_TICKS(ms) ((ms) / portTICK_PERIOD_MS)

// Replace with your Wi-Fi credentials
const char* WIFI_SSID = "REPLACE_WITH_WIFI_SSID";
const char* WIFI_PASSWORD = ""REPLACE_WITH_WIFI_PASSWORD";

// Replace with your Firebase credentials
const char* WEB_API_KEY = "REPLACE_WITH_FIREBASE_PROJECT_API";
const char* DATABASE_URL = "REPLACE_WITH_RTDB_URL";
const char* USER_EMAIL = "REPLACE_WITH_USER_EMAIL";
const char*USER_PASS = "REPLACE_WITH_USER_PASSWORD";

// Set serial monitor baud rate.
static const int BAUD_RATE = 115200;

// Define the GPIO pins used for I2C and SPI communication
static const uint8_t MY_SDA = 21;
static const uint8_t MY_SCL = 22;
static const uint8_t SD_CS = 5;
static const uint8_t LED = LED_BUILTIN;

// Display and sensor configuration constants.
static const uint8_t SCREEN_ADDRESS = 0x3C; // I2C address for the SSD1306 display. Can also be 0x3D for some displays.
static const uint8_t SENSOR_ADDRESS = 0x76; // I2C address for the BMP280 sensor. Can also be 0x77 for some sensors.
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const int8_t OLED_RESET = -1; // Reset pin (-1 if sharing Arduino reset pin)
static const uint8_t DISPLAY_CURSOR_X = 0;
static const uint8_t DISPLAY_CURSOR_Y = 0;
static const uint8_t DISPLAY_TEXT_SIZE = 2;
static const uint16_t DISPLAY_TEXT_COLOR = SSD1306_WHITE;

// Define the intervals for various tasks in milliseconds.
static const int WIFI_CONNECT_INTERVAL_MS = 500;
static const int SENSOR_READ_INTERVAL_MS = 1000;
static const int SERIAL_READ_INTERVAL_MS = 100;
static const int DISPLAY_UPDATE_INTERVAL_MS = 1000;
static const int SDCARD_SAMPLE_INTERVAL_MS = 1000; 
static const int FIREBASE_SAMPLE_INTERVAL_MS = 1000; 
static const int NO_ERROR_LED_INTERVAL_MS = 2500;
static const int HW_ERROR_LED_INTERVAL_MS = 500;
static const int HARDWARE_CHECK_INTERVAL_MS = 5000;

// How long a task will wait in Milliseconds to acquire a mutex before giving up.
static const int SENSOR_MUTEX_WAIT_MS = 10;
static const int I2C_MUTEX_WAIT_MS = 100;
static const int SPI_MUTEX_WAIT_MS = 100;

// Maximum number of samples to average for SD card and Firebase uploads.
static const uint8_t MAX_SDCARD_SAMPLES = 30;    // Number of samples to average for one SD card log.
static const uint8_t MAX_FIREBASE_SAMPLES = 60;  // Number of samples to average for one Firebase upload.

// Buffer sizes for serial input and SD card paths.
static const uint8_t SERIAL_BUFFER_SIZE = 20;
static const uint8_t SD_CARD_FOLDER_PATH_SIZE = 20;
static const uint8_t SD_CARD_FILE_PATH_SIZE = 40;
static const uint8_t SD_CARD_TIME_SIZE = 10;

// These handles are used by the systemMonitor to manage the lifecycle of other tasks.
static TaskHandle_t systemMonitor_h = NULL;
static TaskHandle_t readSensor_h = NULL;
static TaskHandle_t displayData_h = NULL;
static TaskHandle_t sdCardLogger_h = NULL;
static TaskHandle_t readSerial_h = NULL;
static TaskHandle_t firebaseUpload_h = NULL;
static TaskHandle_t firebaseBackground_h = NULL;

// These mutexes are used to protect shared resources from concurrent access.
static SemaphoreHandle_t sensor_mutex;  // Protects the global sensor_data struct 
static SemaphoreHandle_t i2c_mutex;     // Protects the shared I2C hardware bus used by the sensor and display
static SemaphoreHandle_t spi_mutex;     // Protects the shared I2C hardware bus used by the SD card

// Firebase objects and authentication for asynchronous operations.
UserAuth user_auth(WEB_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp firebase;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient async_client(ssl_client);
RealtimeDatabase database;
AsyncResult dbResult;

// Hardware device objects.
Adafruit_BMP280 bmp;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// This struct defines the format for a single sensor reading.
typedef struct {
  float temperature;
  float pressure;
} SensorData_t;

// The global shared data structure that holds the latest sensor reading.
// This is protected by the 'sensor_mutex'.
static SensorData_t sensor_data;

// Flag to indicate if the hardware is functioning correctly.
bool hardware_ok = true; 


//===========================================================================================
//                                     Helper Functions
//===========================================================================================


// Converts Celsius to Fahrenheit.
// This is a simple conversion formula: F = C * 9/5 + 32
float toFahrenheit(float celsius) {
  return celsius * 9.0 / 5.0 + 32.0;
}


// Calculates the average temperature by iterating over an array of SensorData_t.
// It returns 0.0 if the data count is zero to avoid division by zero.
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


// Calculates the average pressure by iterating over an array of SensorData_t.
// It returns 0.0 if the data count is zero to avoid division by zero.
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


// Converts a month number (0-11) to its corresponding name.
// And returns it as a string.
// It returns "Unknown" if the month number is invalid.
const char* getMonthName(int month) {
  const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (month >= 0 && month < 12) {
    return months[month];
  }
  return "Unknown";
}

// Some libraries like Adafruit_SSD1306 might not give an error if the device is not connected.
// This function checks if a device is connected by attempting to begin communication with it.
// at the specified I2C address.
bool deviceConnected(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Checks the hardware status by verifying the presence of the BMP280 sensor and the SSD1306 display.
// It uses the I2C mutex to ensure thread safety while accessing these devices.
// If either device is not found, it prints an error message to the serial monitor and returns false.
// If both devices are found, it returns true.
// This function is called periodically to ensure the hardware is functioning correctly.
bool checkHardware() {
  // Flag to indicate if hardware is functioning correctly.
  bool bmp_ok = false, display_ok = false, sd_card_ok = false;

  // Acquire the I2C mutex to safely access the BMP280 sensor and SSD1306 display.
  if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS) == pdTRUE)) {
    // Reset the I2C bus to ensure clean state
    Wire.end();
    Wire.begin(MY_SDA, MY_SCL);

    // Check if the BMP280 sensor is connected.
    if (deviceConnected(SENSOR_ADDRESS)) {
      bmp_ok = bmp.begin(SENSOR_ADDRESS);
    }
    // Check if the SSD1306 display is connected.
    if (deviceConnected(SCREEN_ADDRESS)) {
      display_ok = display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    }
      
    // Release the I2C mutex after checking the devices.
    xSemaphoreGive(i2c_mutex);
  }

  // Acquire the SPI mutex to safely access the SD card.
  if (xSemaphoreTake(spi_mutex, MS_TO_TICKS(SPI_MUTEX_WAIT_MS)) == pdTRUE) {
    //
    if (SD.begin(SD_CS)) {
      sd_card_ok = true;
    }

    // Release the SPI mutex after checking the SD card.
    xSemaphoreGive(spi_mutex);
  }

  // Print the status of the hardware devices to the serial monitor.
  if (!bmp_ok) {
    Serial.println("System Monitor: BMP280 sensor not found.");
  }
  if (!display_ok) {
    Serial.println("System Monitor: SSD1306 display not found.");
  }
  if (!sd_card_ok) {
    Serial.println("System Monitor: SD card not found.");
  }

  return bmp_ok && display_ok && sd_card_ok;
}

// Lists the available commands for the user in the serial monitor.
// This function is called when the user enters the "Help" command in the serial monitor.
void listAvailableCommands() {
  Serial.println("------------ Available Commands ------------");
  Serial.println("1. Stop            - Suspend all tasks.");   
  Serial.println("2. Stop Display    - Suspend display task.");
  Serial.println("3. Stop SD Card    - Suspend sd card task.");
  Serial.println("4. Stop Firebase   - Suspend firebase task.");
  Serial.println("5. Start           - Resume all tasks.");    
  Serial.println("6. Start Display   - Resume display task."); 
  Serial.println("7. Start SD Card   - Resume sd card task."); 
  Serial.println("8. Start Firebase  - Resume firebase task.");
  Serial.println("9. Help            - List available commands.");
}

// Suspends a task by its handle.
// It checks if the task has been created and then calls vTaskSuspend to suspend the task.
bool suspendTask(TaskHandle_t handle, const char* taskName) {
  if (handle == NULL) {
    Serial.printf("Serial Task: Cannot suspend '%s', task not created.\n", taskName);
    return false;
  }
  vTaskSuspend(handle);
  return true;
}


// Resumes a task by its handle.
// It checks if the task has been created and then calls vTaskResume to resume the task.
bool resumeTask(TaskHandle_t handle, const char* taskName) {
  if (handle == NULL) {
    Serial.printf("Serial Task: Cannot resume '%s', task not created.\n", taskName);
    return false;
  }
  vTaskResume(handle);
  return true;
}


// Suspends all tasks related to the system.
// This function is called when the user enters the "Stop" command in the serial monitor.
// It suspends the read sensor, display, SD card logger, and Firebase upload tasks.
void suspendAllTasks() {
  Serial.println("System Monitor: Suspending all tasks.");
  suspendTask(readSensor_h, "Read Sensor");
  suspendTask(displayData_h, "Display Data");
  suspendTask(sdCardLogger_h, "SD Card Logger");
  suspendTask(firebaseUpload_h, "Firebase Upload");
}


// Resumes all tasks related to the system.
// This function is called when the user enters the "Start" command in the serial monitor.
// It resumes the read sensor, display, SD card logger, and Firebase upload tasks.
void resumeAllTasks() {
  Serial.println("System Monitor: Resuming all tasks.");
  resumeTask(readSensor_h, "Read Sensor");
  resumeTask(displayData_h, "Display Data");
  resumeTask(sdCardLogger_h, "SD Card Logger");
  resumeTask(firebaseUpload_h, "Firebase Upload");
}

// Processes the serial input command.
// It checks the command against a list of known commands and performs the corresponding action.
// If the entered command is not recognized, it prints an error message.
void processSerialInput(char* input) {
  // Use strcasecmp to compare the input command with known commands.
  // I used strcasecmp to make the command case-insensitive.
  if (strcasecmp(input, "Help") == 0) {
    listAvailableCommands();
  }
  else if (strcasecmp(input, "Start") == 0) {
    resumeAllTasks();
  }
  else if (strcasecmp(input, "Start Display") == 0) {
    if (resumeTask(displayData_h, "Display Data")) {
      Serial.println("Serial Task: Display task resumed.");
    }
  }
  else if (strcasecmp(input, "Start SD Card") == 0) {
    if (resumeTask(sdCardLogger_h, "SD Card Logger")) {
      Serial.println("Serial Task: SD Card task resumed.");
    }
  }
  else if (strcasecmp(input, "Start Firebase") == 0) {
    if (resumeTask(firebaseUpload_h, "Firebase Upload")) {
      Serial.println("Serial Task: Firebase task resumed.");
    }
  }
  else if (strcasecmp(input, "Stop") == 0) { 
    suspendAllTasks();
  }
  else if (strcasecmp(input, "Stop Display") == 0) {
    if (suspendTask(displayData_h, "Display Data")) {
      Serial.println("Serial Task: Display task suspended.");
    }
  }
  else if (strcasecmp(input, "Stop SD Card") == 0) {
    if( suspendTask(sdCardLogger_h, "SD Card Logger")) {
      Serial.println("Serial Task: SD Card task suspended.");
    }
  }
  else if (strcasecmp(input, "Stop Firebase") == 0) {
    if (suspendTask(firebaseUpload_h, "Firebase Upload")) {
      Serial.println("Serial Task: Firebase task suspended.");
    }
  }
  else {
    Serial.printf("Serial Task: Unknown command '%s'. Type 'Help' for list of commands.\n", input);
  }
}


//===========================================================================================
//                                     Read Sensor Task
//===========================================================================================


// When a i2c_mutex is acquired this task reads the temperature and pressure from the BMP280 sensor to a local SensorData_t.
// It then updates the global sensor_data struct with the latest readings once it has acquired the sensor_mutex.
// Both these mutexes ensure that the sensor data is read and updated safely without concurrent access issues.
// This task runs at fixed intervals defined by SENSOR_READ_INTERVAL_MS.
void readSensor(void* p) {
  // Local variable to hold the latest sensor data.
  SensorData_t fresh_sensor_data;

  while(1) {
    // Acquire the I2C mutex to safely read from the BMP280 sensor.
    if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS)) == pdTRUE) {
      // Check if the hardware is functioning correctly.
      if (!hardware_ok) {
        continue;
      }

      // Read the temperature and pressure from the BMP280 sensor.
      fresh_sensor_data.temperature = bmp.readTemperature();
      fresh_sensor_data.pressure = bmp.readPressure() / 100.0;

      // Release the I2C mutex after reading the sensor data.
      xSemaphoreGive(i2c_mutex);
    }

    // Acquire the sensor mutex to safely update the global sensor_data struct.
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      // Update the global sensor_data struct with the latest readings.
      sensor_data.temperature = fresh_sensor_data.temperature;
      sensor_data.pressure = fresh_sensor_data.pressure;
      
      // Release the sensor mutex after updating the global sensor_data struct.
      xSemaphoreGive(sensor_mutex);
    }

    vTaskDelay(MS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
  }
}


//===========================================================================================
//                                      Display Task
//===========================================================================================


// This task updates the SSD1306 display with the latest sensor data.
// It first acquires the sensor_mutex to safely read the latest sensor_data to a local SensorData_t.
// It then acquires the i2c_mutex to ensure safe access to the display
// It then clears the display, sets the text color and size, and then prints the temperature and pressure readings.
// It runs at fixed intervals defined by DISPLAY_UPDATE_INTERVAL_MS.
void displayData(void* p) {
  // Local variable to hold the latest sensor data.
  SensorData_t local_sensor_data;

  while(1) {
    // Acquire the sensor mutex to safely read the latest sensor data.
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      // Copy the latest sensor data to a local variable.
      local_sensor_data = sensor_data;

      // Release the sensor mutex after reading the data.
      xSemaphoreGive(sensor_mutex);
    }

    // Acquire the i2c mutex to safely access the display.
    if (xSemaphoreTake(i2c_mutex, MS_TO_TICKS(I2C_MUTEX_WAIT_MS)) == pdTRUE) {
      // Check if the hardware is functioning correctly.
      if (!hardware_ok) {
        xSemaphoreGive(i2c_mutex);
        continue;
      }

      // Clear the display and set the text color and size.
      display.clearDisplay();
      display.setTextColor(DISPLAY_TEXT_COLOR);
      display.setTextSize(DISPLAY_TEXT_SIZE);
      display.setCursor(DISPLAY_CURSOR_X, DISPLAY_CURSOR_Y);

      display.println("  BMP280:");

      // Print the temperature reading in Celsius
      display.print(local_sensor_data.temperature); 
      display.println(" C");

      // Print the temperature reading in Fahrenheit
      display.print(toFahrenheit(local_sensor_data.temperature)); 
      display.println(" F");

      // Print the pressure reading in hPa
      display.print(local_sensor_data.pressure); 
      display.println(" hPa");

      display.display();

      // Release the i2c mutex after updating the display.
      xSemaphoreGive(i2c_mutex);
    }

    vTaskDelay(MS_TO_TICKS(DISPLAY_UPDATE_INTERVAL_MS));
  }
}


//===========================================================================================
//                                     SD Card Task
//===========================================================================================


// This task logs sensor data to an SD card at fixed intervals.
// It first acquires the sensor_mutex to safely read the latest sensor_data to a local array of SensorData_t.
// Once the local array reaches the maximum number of samples defined by MAX_SDCARD_SAMPLES
// it calculates the average temperature and pressure from the local array.
// It then acquires the spi_mutex to ensure safe access to the SD card.
// It then writes the average sensor data to the file in CSV format along with time
// to a folder named with the current month and year, and a file named with the current day, month, and year.
// If the folder or file does not exist, it creates them.
void sdCardLogger(void* p) {
  // Local array to hold sensor data samples for averaging.
  SensorData_t local_sensor_data[MAX_SDCARD_SAMPLES];
  uint8_t sensor_data_count = 0;

  // Local variable to hold the average sensor data.
  SensorData_t avg_sensor_data;

  while(1) {
    // Acquire the sensor mutex to safely read the latest sensor data into local array.
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      // Copy the latest sensor data to the local array.
      local_sensor_data[sensor_data_count++] = sensor_data;

      xSemaphoreGive(sensor_mutex);
    }

    if (sensor_data_count == MAX_SDCARD_SAMPLES) {
      //  If we have collected enough samples calculate the averages.
      avg_sensor_data.temperature = calculateAverageTemp(local_sensor_data, sensor_data_count);
      avg_sensor_data.pressure = calculateAveragePressure(local_sensor_data, sensor_data_count);
      sensor_data_count = 0;

      // Get the current time to use in file.
      struct tm time_info;
      if (!getLocalTime(&time_info)) {
        Serial.println("SD Card Task: Failed to get time. Skipping log.");
        continue;
      }
      
      // Create the folder and file paths based on the current month and year.
      char folder_path[SD_CARD_FOLDER_PATH_SIZE];
      snprintf(folder_path, sizeof(folder_path), "/%s_%d", getMonthName(time_info.tm_mon), time_info.tm_year + 1900);

      // Create the file path with the current day, month, and year.
      char file_path[SD_CARD_FILE_PATH_SIZE];
      snprintf(file_path, sizeof(file_path), "%s/%d_%s_%d.csv", folder_path, time_info.tm_mday, getMonthName(time_info.tm_mon), time_info.tm_year + 1900);

      // Acquire the SPI mutex to safely access the SD card.
      if (xSemaphoreTake(spi_mutex, MS_TO_TICKS(SPI_MUTEX_WAIT_MS)) == pdTRUE) {
        // Check if the hardware is functioning correctly.
        if (!hardware_ok) {
          continue;
        }

        // Check if valid folder exists if not create it.
        if (!SD.exists(folder_path)) {
          // If folder still doesn't get created skig log.
          if (!SD.mkdir(folder_path)) {
            Serial.println("SD Card Task: Folder creation failed. Skipping log.");
            xSemaphoreGive(spi_mutex);
            continue;
          }
        }

        // Check if valid file exists if not create it.
        if (!SD.exists(file_path)) {
          File file = SD.open(file_path, FILE_WRITE);
          // If file is created successfully write header.
          if (file) {
            file.println("Time,Temperature_C,Temperature_F,Pressure_hPa");
            file.close();
          }
          // If file creation fails skip log.
          else {
            Serial.println("SD Card Task: File creation failed. Skipping log.");
            xSemaphoreGive(spi_mutex);
            continue;
          }
        }

        // Open the file in append mode to write the average sensor data.
        File file = SD.open(file_path, FILE_APPEND);
        if (file) {
          char time[SD_CARD_TIME_SIZE];
          // Format the current time as HH:MM:SS.
          strftime(time, sizeof(time), "%H:%M:%S", &time_info);

          // Write the average sensor data to the file in CSV format.
          Serial.println("SD Card Task: Writing data to SD card.");
          file.printf("%s,%.2f,%.2f,%.2f\n", time, avg_sensor_data.temperature, toFahrenheit(avg_sensor_data.temperature), avg_sensor_data.pressure);
          file.close();
        }
        // If file write fails print error and skip log.
        else {
          Serial.println("SD Card Task: File write failed. Skipping log.");
          xSemaphoreGive(spi_mutex);
          continue;
        }

        // Release the SPI mutex after writing to the SD card.
        xSemaphoreGive(spi_mutex);
      }
    }

    vTaskDelay(MS_TO_TICKS(SDCARD_SAMPLE_INTERVAL_MS));
  }
}


//===========================================================================================
//                                    Firebase Task
//===========================================================================================


// This task uploads sensor data to Firebase at fixed intervals.
// It first acquires the sensor_mutex to safely read the latest sensor_data to a local array of SensorData_t.
// Once the local array reaches the maximum number of samples defined by MAX_FIREBASE_SAMPLES
// it calculates the average temperature and pressure from the local array.
// It then uploads the average sensor data to Firebase under the defined FIREBASE_PATH.
// It uses the FirebaseClient library's asynchronous API to perform the upload.
// If Firebase is not ready, it skips the upload and prints a message to the serial monitor
void firebaseUpload(void* p) {
  // Local array to hold sensor data samples for averaging.
  SensorData_t local_sensor_data[MAX_FIREBASE_SAMPLES];
  uint8_t sensor_data_count = 0;

  // Local variable to hold the average sensor data.
  SensorData_t avg_sensor_data;

  while(1) {
    // Check if the hardware is functioning correctly.
    if (!hardware_ok) {
      continue;
    }

    // Acquire the sensor mutex to safely read the latest sensor data into local array.
    if (xSemaphoreTake(sensor_mutex, MS_TO_TICKS(SENSOR_MUTEX_WAIT_MS)) == pdTRUE) {
      local_sensor_data[sensor_data_count++] = sensor_data;

      // Release the sensor mutex after reading the data.
      xSemaphoreGive(sensor_mutex);
    }

    // If we have collected enough samples calculate the averages and upload to Firebase.
    if (sensor_data_count == MAX_FIREBASE_SAMPLES) {
      // Check if Firebase is ready before proceeding with upload.
      if (firebase.ready()) {
        // Get the current time to use in the upload.
        struct tm time_info;
        if (!getLocalTime(&time_info)) {
          Serial.println("Firebase Task: Failed to get time. Skipping log.");
          sensor_data_count = 0;
          continue;
        }

        // Calculate the average temperature and pressure from the local sensor data.
        avg_sensor_data.temperature = calculateAverageTemp(local_sensor_data, sensor_data_count);
        avg_sensor_data.pressure = calculateAveragePressure(local_sensor_data, sensor_data_count);

        // Create the log entry path based on the current time.
        // Year/Month/Day/Hour_Minute_Second
        char base_path[30];
        snprintf(base_path, sizeof(base_path), 
         "/%d/%s/%d/%02d_%02d_%02d", 
         time_info.tm_year + 1900,          // Year
         getMonthName(time_info.tm_mon),    // Month name
         time_info.tm_mday,                 // Day
         time_info.tm_hour,                 // Hour
         time_info.tm_min,                  // Minute
         time_info.tm_sec);                 // Second

        char full_path[45];

        Serial.println("Firebase Task: Sending data to Firebase.");

        // Create the full path for temperature in Celsius and send to Firebase.
        strcpy(full_path, base_path);
        strcat(full_path, "/temperature_c");
        database.set<float>(async_client, full_path, avg_sensor_data.temperature, dbResult);
        
        // Create the full path for temperature in Fahrenheit and send to Firebase.
        strcpy(full_path, base_path);
        strcat(full_path, "/temperature_f");
        database.set<float>(async_client, full_path, toFahrenheit(avg_sensor_data.temperature), dbResult);

        // Create the full path for pressure and send to Firebase.
        strcpy(full_path, base_path);
        strcat(full_path, "/pressure_hpa");
        database.set<float>(async_client, full_path, avg_sensor_data.pressure, dbResult);
      }
      // If Firebase is not ready print a message to the serial monitor and skip the upload.
      else {
        Serial.println("Firebase Task: Firebase not ready. Skipping upload.");
      }

      // Reset the sensor data count after uploading.
      sensor_data_count = 0;
    }

    vTaskDelay(MS_TO_TICKS(FIREBASE_SAMPLE_INTERVAL_MS));
  }
}

void firebaseBackground(void * p) {
  while(1) {
    firebase.loop();
  }
}

//===========================================================================================
//                                   Read Serial Task
//===========================================================================================


// This task reads input from the serial monitor.
// It waits for the user to enter a command and processes it.
// It uses a buffer to store the input until the user presses Enter (newline character).
// It then sends the command to the processSerialInput function for processing.
// It supports commands to start and stop tasks, display available commands, and more.
void readSerial(void* p) {
  // Define a buffer to store the serial input.
  char buffer[SERIAL_BUFFER_SIZE];
  // Define a character to store serial input character by character.
  char ch;
  // Define an index to keep track of the current position in the buffer.
  uint8_t index = 0;

  // Initialize the buffer to be empty.
  memset(buffer, 0, SERIAL_BUFFER_SIZE); 

  while(1) {
    // Check if there is data available in the serial buffer.
    // If there is store it in the character variable 'ch'.
    if (Serial.available()) {
      ch = Serial.read();

      // If the character is a newline end the buffer  with '\0'
      if (ch == '\n' || ch == '\r') {
        buffer[index] = '\0';
        
        Serial.println(buffer);

        // Process the input.
        processSerialInput(buffer);

        // Handle any remaining characters in the buffer after the newline.
        while (Serial.available()) {
          Serial.read();
        }   

        // Index for the next input.
        index = 0;
      }
      // Handle backspaces by removing the last character from the buffer.
      else if (ch == '\b' || ch == 127) {
        if (index > 0) {
          index--;
          Serial.print("\b \b");
        }
      }
      else {
        // If the character is not a newline, store it in the buffer.
        // but only if there is enough space left in the buffer.
        if (index < SERIAL_BUFFER_SIZE - 1) {
          buffer[index++] = ch;
        }
      }
    }

    vTaskDelay(MS_TO_TICKS(SERIAL_READ_INTERVAL_MS));
  }
}


//===========================================================================================
//                                  System Monitor Task
//===========================================================================================


// This is the main system monitor task that manages the overall system state.
// It checks the hardware status, initializes tasks, and monitors the system state.
// It uses a tate machine to handle different states: HARDWARE_INIT, HARDWARE_ERROR, and RUNNING.
// In the HARDWARE_INIT state, it checks the hardware and initializes tasks if the hardware is OK.
// In the HARDWARE_ERROR state, it blinks an LED at a specified rate to indicate an error and checks
// the hardware status periodically to see if it has recovered.
// In the RUNNING state, it blinks the LED at a different rate to indicate normal operation and checks
// the hardware status periodically to ensure everything is functioning correctly.
// It also connects to Wi-Fi and initializes Firebase.
void systemMonitor(void* p) {
  // Define the system states.
  // And initialize the system state to HARDWARE_INIT.
  typedef enum {HARDWARE_INIT, HARDWARE_ERROR, RUNNING} SystemState_t;
  static SystemState_t system_state = HARDWARE_INIT;

  // Start the timer to track hardware check intervals.
  TickType_t hardware_check_start_time = xTaskGetTickCount();

  // Initialize the tasks running flag to false.
  // This flag is used to track whether the tasks have been initialized or not
  // so that they are not initialized multiple times.
  bool tasks_running = false;

  // Initialize Wi-Fi and connect to the network.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("\nSystem Monitor: Connecting to Wi-Fi");
  // Wait for the Wi-Fi connection to be established.
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(MS_TO_TICKS(WIFI_CONNECT_INTERVAL_MS));
  }
  Serial.println();

  // Synchronize the system time using NTP servers.
  // This is important for timestamping the Firebase and SD card logs.
  Serial.print("System Monitor: Synchronizing system time.");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    vTaskDelay(MS_TO_TICKS(WIFI_CONNECT_INTERVAL_MS));
  }
  Serial.println();

  // Configure SSL client for Firebase.
  ssl_client.setInsecure();

  // Initialize Firebase with the provided credentials and database URL.
  Serial.println("System Monitor: Initializing Firebase.");
  initializeApp(async_client, firebase, getAuth(user_auth), NULL, "authTask");
  firebase.getApp<RealtimeDatabase>(database);
  database.url(DATABASE_URL);

  while(1) {
    switch (system_state) {
      case HARDWARE_INIT:
        // Check the hardware status.
        if (checkHardware()) {
          // If the hardware is OK and tasks are not already running initialize the tasks.
          if (!tasks_running) {
            Serial.println("System Monitor: Initializing system.");

            // Replace 'xTaskCreatePinnedToCore' with 'xTaskCreate' 
            // if using vanilla FreeRTOS and remove the core parameter.
            // Here I'm using a modifed version of FreeRTOS 
            // by ESP which allows pinning tasks to cores.
            // This is because ESP32 has 2 cores as opposed to 1 core in vanilla FreeRTOS.
            xTaskCreatePinnedToCore(readSensor, "Read Sensor", 2048, NULL, 4, &readSensor_h, 0);
            xTaskCreatePinnedToCore(displayData, "Display Data", 2048, NULL, 3, &displayData_h, 0);
            xTaskCreatePinnedToCore(sdCardLogger, "SD Card Logger", 4096, NULL, 2, &sdCardLogger_h, 0);
            xTaskCreatePinnedToCore(readSerial, "Read Serial", 4096, NULL, 3, &readSerial_h, 1);
            xTaskCreatePinnedToCore(firebaseUpload, "Firebase Upload", 8192, NULL, 2, &firebaseUpload_h, 1);
            xTaskCreatePinnedToCore(firebaseBackground, "Firebase Background", 8192, NULL, 1, &firebaseBackground_h, 1);
            tasks_running = true;
            Serial.println("System Monitor: System running.");
          }
          // Otherwise resume the tasks if they were suspended.
          else {
            Serial.println("System Monitor: Hardware recovery successful.");
            resumeAllTasks();
            resumeTask(readSerial_h, "Read Serial");
            Serial.println("System Monitor: System running.");
          }

          // Set the system state to RUNNING and reset the hardware check timer.
          system_state = RUNNING;
          hardware_ok = true;
          hardware_check_start_time = xTaskGetTickCount();
        }
        // If the hardware is not OK set the system state to HARDWARE_ERROR and start the hardware check timer.
        else {
          system_state = HARDWARE_ERROR;
          hardware_ok = false;
          hardware_check_start_time = xTaskGetTickCount();
        }
        break;

      case HARDWARE_ERROR:
        // Blink the LED at a defined interval to indicate hardware error.
        digitalWrite(LED, HIGH);
        vTaskDelay(MS_TO_TICKS(100));
        digitalWrite(LED, LOW);
        vTaskDelay(MS_TO_TICKS(HW_ERROR_LED_INTERVAL_MS));

        // If the hardware check timer has run out check the hardware status again.
        if (xTaskGetTickCount() - hardware_check_start_time >= MS_TO_TICKS(HARDWARE_CHECK_INTERVAL_MS)) {
          Serial.println("System Monitor: Checking hardware.");
          // If the hardware is OK set system state to HARDWARE_INIT so that the tasks can be resumed.
          if (checkHardware()) {
            system_state = HARDWARE_INIT;
          }
          // Otherwise if the hardware is still not OK reset the hardware check timer.
          else {
            hardware_check_start_time = xTaskGetTickCount();
          }
        }
        break;

      case RUNNING:
        // Blink the LED at a defined interval to indicate normal operation.
        digitalWrite(LED, HIGH);
        vTaskDelay(MS_TO_TICKS(100));
        digitalWrite(LED, LOW);
        vTaskDelay(MS_TO_TICKS(NO_ERROR_LED_INTERVAL_MS));

        // If the hardware check timer has run out check the hardware status again.
        if (xTaskGetTickCount() - hardware_check_start_time >= MS_TO_TICKS(HARDWARE_CHECK_INTERVAL_MS)) {
          // If the hardware is not OK set system state to HARDWARE_ERROR and suspend all tasks.
          if (!checkHardware()) {
            hardware_ok = false;
            suspendAllTasks();
            suspendTask(readSerial_h, "Read Serial");

            system_state = HARDWARE_ERROR;
          }

          // Reset the hardware check timer.
          hardware_check_start_time = xTaskGetTickCount();
        }
        break;
    }
  }
}


//===========================================================================================
//                                    Setup & Loop
//===========================================================================================


// The setup function initializes the serial monitor, I2C bus, and LED pin.
// It also creates the necessary mutexes for sensor, I2C, and SPI access.
// It then creates the system monitor task which will manage the overall system state and tasks.
void setup() {
  // Initialize serial monitor to the defined baud rate.
  Serial.begin(BAUD_RATE);
  // Initialize I2C bus with the defined SDA and SCL pins.
  Wire.begin(MY_SDA, MY_SCL);
  // Initialize the LED pin as an output.
  pinMode(LED, OUTPUT);

  // Wait for the serial monitor to be ready.
  vTaskDelay(MS_TO_TICKS(1000));

  // Initializes all the mutexes used in the system.
  sensor_mutex = xSemaphoreCreateMutex();
  i2c_mutex = xSemaphoreCreateMutex();
  spi_mutex = xSemaphoreCreateMutex();

  // Create the system monitor task which will manage the overall system state and tasks.
  // It has higher priority than other tasks to so that it can manage the system effectively.
  xTaskCreatePinnedToCore(systemMonitor, "System Monitor", 16384, NULL, 5, &systemMonitor_h, 0);
}

// Do nothing 
void loop() {}