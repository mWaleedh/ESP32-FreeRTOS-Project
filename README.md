# ESP32-FreeRTOS-Weather-Station: A Technical Deep Dive

![Project Demo GIF](placeholder.gif)
*(Recommendation: Replace with a GIF of the project working.)*

## Project Purpose

The primary goal of this project was to move beyond simple, single-threaded programming and gain a deep, practical understanding of a real-time operating system (RTOS). I chose FreeRTOS due to its prevalence in the embedded industry and used this weather station as a platform to implement and master its core concepts in a complex, multi-functional system.

---

## What I Learned

This project served as a hands-on masterclass in embedded systems engineering. The key concepts I implemented and learned are:

-   **FreeRTOS Core Concepts:** Mastered the fundamentals of task scheduling, inter-task communication, and resource management.
-   **Concurrency Primitives:**
    -   **Mutexes:** Implemented mutexes to provide thread-safe access to shared hardware peripherals (I2C bus) and shared data structures (global sensor data), successfully preventing race conditions.
    -   **Task Synchronization:** Designed a system where tasks operate independently but in a coordinated manner.
-   **Advanced RTOS Topics:** Architected the system to avoid common pitfalls such as **deadlock** (by using a non-nested locking policy) and understood the implications of **priority inversion**.
-   **Fault-Tolerant Design:** Implemented a supervisor task that monitors system health, detects runtime hardware failures, and can safely suspend and resume worker tasks, making the system resilient.
-   **Hardware Interfacing:** Gained experience with multiple communication protocols (I2C for sensors/display, SPI for the SD card) in a multi-threaded environment.
-   **Cloud IoT Integration:** Learned to integrate a real-world IoT cloud service (Firebase) into an embedded device, focusing on modern, non-blocking asynchronous communication patterns.

---

## Hardware Used

This project uses a selection of common and versatile components:

-   **ESP32-DOIT-DEVKIT-V1:** A powerful dual-core microcontroller with built-in Wi-Fi and Bluetooth. Its dual-core nature was essential for separating real-time tasks from communication tasks.
-   **BMP280 Barometric Pressure & Temperature Sensor:** An I2C sensor used for gathering environmental data. This sensor provides pressure and temperature. Note that the popular **BME280** sensor, which adds a humidity reading, is a drop-in replacement and would work with this code after minor modifications to the `SensorData_t` struct and the `readSensor` task.
-   **SSD1306 0.96" OLED Display:** A small I2C monochrome display used to provide a real-time, local data readout.
-   **MicroSD Card Module & Card (FAT32):** An SPI-based storage solution used for long-term, offline data logging. The FAT32 file system is used for broad compatibility.

---

## RTOS Architecture: Tasks, Cores, and Priorities

The firmware is built on a carefully designed multi-tasking architecture to ensure stability and real-time performance.

### Core & Priority Assignment

The ESP32's two cores are leveraged to isolate tasks based on their behavior:

-   **Core 0 (Real-Time Core):** Dedicated to time-sensitive and deterministic operations like hardware interfacing. This prevents unpredictable network latency from ever causing a missed sensor reading.
    -   `systemMonitor` (Priority 5 - Supervisor)
    -   `readSensor` (Priority 4 - Data Producer)
    -   `displayData` (Priority 3 - Local I/O)
    -   `sdCardLogger` (Priority 2 - Local I/O)
-   **Core 1 (Application & Comms Core):** Handles non-deterministic, potentially blocking tasks like networking and user input.
    -   `readSerial` (Priority 3 - User Interaction)
    -   `firebaseUpload` (Priority 2 - Networking)

This separation is a key architectural choice for building reliable connected devices. Priorities are assigned based on importance and responsiveness requirements; for example, the `readSensor` task is given a higher priority than the logging tasks to ensure data acquisition is never delayed.

### Task Breakdown & Memory Allocation

-   **`systemMonitor` (4096 bytes):** The highest priority task. It acts as the system supervisor, handling the boot-up sequence, hardware checks, and the lifecycle (creation, suspension, resumption) of all other tasks. It requires a larger stack to manage the Wi-Fi and Firebase initialization.
-   **`readSensor` (2048 bytes):** A simple, periodic task. It wakes up every second, safely acquires the I2C bus lock, reads data from the BMP280, and then safely acquires the data lock to update a global `SensorData_t` struct.
-   **`displayData` (2048 bytes):** A periodic task that updates the OLED display. It safely reads from the global sensor data struct and then locks the I2C bus to perform its drawing operations.
-   **`sdCardLogger` (4096 bytes):** A data processing and logging task. It collects a batch of sensor readings, calculates their average to reduce noise, and writes a single, organized entry to the SD card. It handles the creation of date-stamped folders and files. Requires a larger stack for the filesystem library.
-   **`firebaseUpload` (8192 bytes):** The cloud communication task. Similar to the SD logger, it collects and averages data. It then sends this data to the Firebase Realtime Database using non-blocking, asynchronous API calls. Networking and SSL/TLS libraries require a significant amount of stack space.
-   **`readSerial` (2048 bytes):** Manages the Command-Line Interface (CLI). It listens for user input and executes commands like suspending or resuming other tasks.

---

## Firebase Integration & Open Source Contribution

A major part of this project was learning to integrate a modern IoT cloud service. I chose the `Firebase-ESP-Client` library by `mobizt` for its powerful asynchronous features, which are essential for a non-blocking RTOS environment.

While learning about the library, I was looking through its documentation and official examples. This led me to finding and fixing several minor errors in the repository's README files and example code. I was excited to have **multiple pull requests merged** by the project's author, allowing me to contribute back to the open-source community and also becoming one of the few contributers of this powerful library.

---

## How to Set Up and Run This Project

This project is built using **PlatformIO** within **Visual Studio Code**, which is the recommended development environment.

### Step 1: Install Software
1.  Download and install [Visual Studio Code](https://code.visualstudio.com/).
2.  Open VS Code, go to the Extensions tab, and search for and install the official **PlatformIO IDE** extension.

### Step 2: Download the Code
1.  You will need [Git](https://git-scm.com/downloads) installed on your computer.
2.  Open a terminal or command prompt and clone this repository using the following command:
    ```bash
    git clone https://github.com/mWaleedh/ESP32-FreeRTOS-Project.git
    ```

### Step 3: Setup Firebase Project

This project requires a Google Firebase project to store and manage the IoT data. 

For a more detailed, visual guide, you can also follow the excellent tutorial at **[Random Nerd Tutorials](https://randomnerdtutorials.com/esp32-firebase-realtime-database/)**. Alternatively, you can continue with the steps below.

1.  **Create a Firebase Project:**
    *   Go to the [Firebase Console](https://console.firebase.google.com/).
    *   Click on **"Add project"** and give it a unique name.
    *   You can disable Google Analytics for this project to simplify the setup.
    *   Click **"Create project"**.

2.  **Create a Realtime Database:**
    *   From your new project's dashboard, go to the **"Build"** section in the left-hand menu and click on **"Realtime Database"**.
    *   Click the **"Create Database"** button.
    *   Choose a location for your database (choose the one closest to you).
    *   Select **"Start in test mode"**. This sets the security rules to allow reads and writes while you are developing. 
    *   **[!] Security Warning:** Test mode allows anyone with your database URL to read and write data. It is great for development but should **never** be used for a production project.
    *   Click **"Enable"**.
    *   Once created, you will see your **Database URL** at the top (it looks like `https://project-name-default-rtdb.firebaseio.com/`). **Copy this URL.** You will need it later.

3.  **Enable Email/Password Authentication:**
    *   Go back to the **"Build"** section and click on **"Authentication"**.
    *   Click the **"Get started"** button.
    *   In the "Sign-in method" tab, click on **"Email/Password"** from the list of providers.
    *   **Enable** the first toggle switch and click **"Save"**.

4.  **Create a User for Your Device:**
    *   While still in the Authentication section, go to the **"Users"** tab.
    *   Click the **"Add user"** button.
    *   Enter an email and a password. This will be the dedicated "account" for your ESP32 device. **Copy the email and password.** You will need them later.

5.  **Find Your Web API Key:**
    *   In the top-left of the Firebase console, click the gear icon next to "Project Overview" and select **"Project settings"**.
    *   Under **Your Project** Copy the `apiKey` value. You will need it later.

### Step 4: Configure Project Credentials
1.  Open the cloned folder in VS Code. When prompted, trust the folder.
2.  Navigate to the `src/main.cpp` file.
3.  At the top of the file, find the following constants and replace the placeholder values with your own:
    -   `WIFI_SSID`
    -   `WIFI_PASSWORD`
    -   `WEB_API_KEY` (Your Firebase Project's Web API Key)
    -   `DATABASE_URL` (Your Firebase Realtime Database URL)
    -   `USER_EMAIL` (The email for your Firebase authentication user)
    -   `USER_PASS` (The password for your Firebase authentication user)

### Step 5: Build and Upload
1.  PlatformIO will automatically detect the `platformio.ini` file and download all the required libraries (like Adafruit sensor libraries and the Firebase client).
2.  Connect your ESP32 board to your computer via USB.
3.  In the PlatformIO toolbar at the bottom of the VS Code window, click the **"Upload"** button (it looks like a right-facing arrow). PlatformIO will compile the code and flash it to your device.

### Step 6: Run and Interact
1.  After the upload is complete, click the **"Monitor"** button (it looks like a plug) in the PlatformIO toolbar.
2.  This will open the Serial Monitor at 115200 baud. You will see the system boot up, connect to Wi-Fi, and perform its hardware checks.
3.  If nothing is displayed on the Serial Monitor press the restart button on your ESP32 board to restart the system.
4.  Type `Help` into the monitor and press Enter to see a list of available CLI commands to interact with the running system.
