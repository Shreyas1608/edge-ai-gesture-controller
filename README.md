# Edge-AI Gesture Controller

A low-latency, multi-modal human-computer interface that bridges physical hardware actuation and digital software triggers using real-time hand gesture recognition. An ESP32-CAM node streams live MJPEG video over Wi-Fi to a Python host running MediaPipe and OpenCV, which classifies hand landmarks and dispatches asynchronous HTTP commands back to the edge node.

## Key Features

* **Byte-Level MJPEG Stream Parser:** Bypasses standard OpenCV/FFmpeg video wrappers to decode raw JPEG frames directly, eliminating stream overread crashes and automatically flushing corrupted packets.
* **Zero-Overhead UI & Inference:** Runs MediaPipe 21-point hand landmark estimation on downscaled `320x240` frames for ~3x faster CPU throughput, while pre-rendering colored Unicode emojis via Pillow into cached NumPy patches for instant blitting.
* **Socket-Safe Asynchronous Networking:** Uses a background `queue.Queue` worker with state deduplication and `Connection: close` headers to prevent TCP socket exhaustion on the single-threaded ESP32 web server.
* **Multi-Modal Control:** Simultaneously actuates physical peripherals (SG90 servo motor, status LEDs) and triggers on-screen software states.

---

## Gesture Mapping

| Gesture | Servo Angle (GPIO 13) | Green LED (GPIO 12) | Flash LED (GPIO 4) | UI Emoji | Software Trigger |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Open Palm** | 90° | ON | OFF | ✋ | Standby / Idle |
| **Fist** | 0° | OFF | OFF | ✊ | System Locked |
| **Pointing** | 45° | OFF | OFF | ☝️ | Item Selected |
| **Peace / V-Sign** | 135° | OFF | ON | ✌️ | Trigger Capture |
| **Three Fingers** | 180° | ON | ON | 🤟 | Switch Workspace |
| **Thumbs Up** | 90° | ON | ON | 👍 | Operation Confirmed |

---

## Hardware & Wiring

### Peripheral Connections

| Component | Wire / Pin | ESP32-CAM Pin | Notes |
| :--- | :--- | :--- | :--- |
| **SG90 Servo** | Signal (Orange/Yellow) | **GPIO 13** | PWM control signal |
| | Power (Red) | **External 5V** | **Isolate power:** Do not power from the FTDI 5V pin to avoid brownouts |
| | Ground (Brown/Black) | **Common GND** | Must share ground with the ESP32-CAM |
| **Green LED** | Anode (+) | **GPIO 12** | Use a 220Ω–330Ω current-limiting resistor in series |
| | Cathode (-) | **Common GND** | |
| **Flash LED** | Onboard SMD | **GPIO 4** | Built-in; no external wiring needed |

### FTDI Programmer (Flashing)

| FTDI Pin | ESP32-CAM Pin |
| :--- | :--- |
| 5V | 5V (ensure FTDI jumper is set to 5V) |
| GND | GND |
| TX | U0R (GPIO 3) |
| RX | U0T (GPIO 1) |
| — | Bridge **GPIO 0 to GND** during upload; remove to run |

---

## Prerequisites

* **Arduino IDE** with the **ESP32 Board Package** installed (via *Boards Manager* using the Espressif board manager URL).
* **Python 3.8+** installed on the host machine.
* **2.4 GHz Wi-Fi Network:** Both the ESP32-CAM and the host PC must be connected to the exact same **2.4 GHz** network (the ESP32 does not support 5 GHz Wi-Fi).

---

## Setup & Usage

### 1. Flash the ESP32-CAM
1. Connect the ESP32-CAM to your PC using the FTDI programmer (refer to the FTDI wiring table above) and bridge **GPIO 0 to GND**.
2. Open `esp32.ino` in the Arduino IDE.
3. Replace the Wi-Fi placeholders (`YOUR_SSID` and `YOUR_PASSWORD`) with your 2.4 GHz network credentials.
4. Select **AI Thinker ESP32-CAM** under *Tools > Board*, choose the correct COM port, and upload the sketch.
5. Disconnect the **GPIO 0 to GND** jumper, press the **RST** button on the ESP32-CAM, and open the Serial Monitor at **115200 baud** to note the printed IP address.

### 2. Wire the Peripherals
Connect the SG90 servo (using an external 5V power supply with a shared common ground) and the Green LED with a 220Ω–330Ω resistor according to the Peripheral Connections table above.

### 3. Run the Python Host
1. Clone the repository and navigate into the project directory:
   ```bash
   git clone [https://github.com/Shreyas1608/](https://github.com/Shreyas1608/)<repo-name>.git
   cd <repo-name>