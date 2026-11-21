import cv2
from cvzone.HandTrackingModule import HandDetector
import numpy as np
import requests
import threading
import queue
import time
from PIL import Image, ImageDraw, ImageFont

# --- CONFIGURATION ---
ESP32_IP = "http://10.111.23.123"
STREAM_URL = f"{ESP32_IP}:81/stream"
LED_URL = f"{ESP32_IP}/led"
SERVO_URL = f"{ESP32_IP}/servo"

# --- NON-BLOCKING DEDUPED NETWORK WORKER ---
cmd_queue = queue.Queue(maxsize=10)

def network_worker():
    while True:
        url = cmd_queue.get()
        try:
            requests.get(url, timeout=0.8, headers={'Connection': 'close'})
        except Exception:
            pass
        cmd_queue.task_done()

threading.Thread(target=network_worker, daemon=True).start()

def queue_command(url):
    try:
        cmd_queue.put_nowait(url)
    except queue.Full:
        pass

# --- TELEMETRY & CACHED HARDWARE STATE ---
hardware_state = {
    "GPIO_12": "OFF",
    "GPIO_04": "OFF",
    "SERVO_ANGLE": 90
}

software_actions = {
    "Open Palm":      {"emoji": "✋", "action": "STANDBY / IDLE",       "color": (0, 255, 120)},
    "Three Fingers":  {"emoji": "🤟", "action": "SWITCH WORKSPACE",    "color": (255, 0, 255)},
    "Peace / V-Sign":  {"emoji": "✌️", "action": "TRIGGER CAPTURE",     "color": (255, 255, 0)},
    "Pointing":       {"emoji": "☝️", "action": "ITEM SELECTED",       "color": (0, 165, 255)},
    "Thumbs Up":      {"emoji": "👍", "action": "OPERATION CONFIRMED", "color": (0, 255, 0)},
    "Fist":           {"emoji": "✊", "action": "SYSTEM LOCKED",        "color": (0, 0, 255)},
    "None":           {"emoji": "⏳", "action": "AWAITING INPUT",       "color": (150, 150, 150)}
}

current_locked_gesture = "None"
candidate_gesture = "None"
candidate_count = 0
DEBOUNCE_THRESHOLD = 4

# --- PRE-RENDER EMOJIS (ZERO RUNTIME OVERHEAD) ---
try:
    emoji_font = ImageFont.truetype("seguiemj.ttf", 52)
except Exception:
    emoji_font = ImageFont.load_default()

emoji_cache = {}
for key, meta in software_actions.items():
    patch = Image.new("RGB", (70, 70), (20, 20, 20))
    draw = ImageDraw.Draw(patch)
    draw.text((5, 2), meta["emoji"], font=emoji_font, embedded_color=True)
    emoji_cache[key] = cv2.cvtColor(np.array(patch), cv2.COLOR_RGB2BGR)

# --- HIGH-THROUGHPUT BYTE STREAM PARSER ---
class RobustMJPEGStream:
    def __init__(self, url):
        self.url = url
        self.frame = None
        self.running = True
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self):
        while self.running:
            try:
                resp = requests.get(self.url, stream=True, timeout=2.5, headers={'Connection': 'close'})
                buf = bytes()
                for chunk in resp.iter_content(chunk_size=2048):
                    if not self.running:
                        break
                    buf += chunk
                    
                    if len(buf) > 150000:
                        buf = buf[-40000:]
                        continue

                    a = buf.find(b'\xff\xd8')
                    b = buf.find(b'\xff\xd9')
                    
                    if a != -1 and b != -1 and b > a:
                        jpg = buf[a:b+2]
                        buf = buf[b+2:]
                        img = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
                        if img is not None:
                            with self.lock:
                                self.frame = img
            except Exception:
                time.sleep(0.15)

    def read(self):
        with self.lock:
            # .copy() isolates the UI drawing layer from the background network buffer
            return (True, self.frame.copy()) if self.frame is not None else (False, None)

    def release(self):
        self.running = False

def dispatch_hardware(gesture):
    global current_locked_gesture
    if gesture == current_locked_gesture:
        return
    current_locked_gesture = gesture

    if gesture == "Open Palm":
        target_servo, target_g, target_f = 90, "ON", "OFF"
    elif gesture == "Fist":
        target_servo, target_g, target_f = 0, "OFF", "OFF"
    elif gesture == "Pointing":
        target_servo, target_g, target_f = 45, "OFF", "OFF"
    elif gesture == "Peace / V-Sign":
        target_servo, target_g, target_f = 135, "OFF", "ON"
    elif gesture == "Three Fingers":
        target_servo, target_g, target_f = 180, "ON", "ON"
    elif gesture == "Thumbs Up":
        target_servo, target_g, target_f = 90, "ON", "ON"
    else:
        return

    # Fire commands only if state actually changes to avoid flooding the ESP32
    if hardware_state["SERVO_ANGLE"] != target_servo:
        queue_command(f"{SERVO_URL}?angle={target_servo}")
        hardware_state["SERVO_ANGLE"] = target_servo

    if hardware_state["GPIO_12"] != target_g:
        queue_command(f"{LED_URL}?pin=12&state={1 if target_g == 'ON' else 0}")
        hardware_state["GPIO_12"] = target_g

    if hardware_state["GPIO_04"] != target_f:
        queue_command(f"{LED_URL}?pin=4&state={1 if target_f == 'ON' else 0}")
        hardware_state["GPIO_04"] = target_f

# --- INITIALIZATION ---
detector = HandDetector(maxHands=1, detectionCon=0.65)
stream = RobustMJPEGStream(STREAM_URL)
print(f"Tracking connected to: {STREAM_URL}")

while True:
    ret, frame = stream.read()
    if not ret or frame is None:
        if cv2.waitKey(5) & 0xFF == ord('q'):
            break
        continue

    # Downscale for lightweight CPU landmark estimation
    small_frame = cv2.resize(frame, (320, 240))
    hands, _ = detector.findHands(small_frame, draw=False, flipType=False)
    raw_gesture = "No Hand"

    if hands:
        hand = hands[0]
        lmList = hand["lmList"]
        fingers = detector.fingersUp(hand)

        thumb_tip_y = lmList[4][1]
        thumb_ip_y = lmList[3][1]
        index_mcp_x = lmList[5][0]
        thumb_tip_x = lmList[4][0]

        is_thumb_up = (thumb_tip_y < thumb_ip_y) and (abs(thumb_tip_x - index_mcp_x) > 15)
        four_fingers = fingers[1:]

        if four_fingers == [1, 1, 1, 1]:
            raw_gesture = "Open Palm"
        elif four_fingers == [0, 0, 0, 0] and not is_thumb_up:
            raw_gesture = "Fist"
        elif four_fingers == [0, 0, 0, 0] and is_thumb_up:
            raw_gesture = "Thumbs Up"
        elif four_fingers == [1, 0, 0, 0]:
            raw_gesture = "Pointing"
        elif four_fingers == [1, 1, 0, 0]:
            raw_gesture = "Peace / V-Sign"
        elif four_fingers == [1, 1, 1, 0] or (four_fingers == [1, 1, 0, 0] and is_thumb_up):
            raw_gesture = "Three Fingers"
        else:
            raw_gesture = "Unknown"

        # Scale and draw hand box on main frame
        bbox = hand["bbox"]
        bx, by, bw, bh = [coord * 2 for coord in bbox]
        cv2.rectangle(frame, (bx, by), (bx + bw, by + bh), (0, 255, 120), 2)

    # Debounce filter
    if raw_gesture == candidate_gesture:
        candidate_count += 1
    else:
        candidate_gesture = raw_gesture
        candidate_count = 1

    if candidate_count >= DEBOUNCE_THRESHOLD and candidate_gesture not in ("No Hand", "Unknown"):
        dispatch_hardware(candidate_gesture)

    # --- TELEMETRY HUD (HARDWARE) ---
    cv2.rectangle(frame, (10, 10), (320, 120), (20, 20, 20), -1)
    cv2.rectangle(frame, (10, 10), (320, 120), (0, 255, 120), 2)
    cv2.putText(frame, "HARDWARE NODE", (20, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 255), 2)
    
    g_col = (0, 255, 0) if hardware_state['GPIO_12'] == "ON" else (110, 110, 110)
    f_col = (255, 255, 0) if hardware_state['GPIO_04'] == "ON" else (110, 110, 110)
    cv2.putText(frame, f"LED 12 : {hardware_state['GPIO_12']}", (20, 55), cv2.FONT_HERSHEY_SIMPLEX, 0.45, g_col, 1)
    cv2.putText(frame, f"Flash  : {hardware_state['GPIO_04']}", (20, 75), cv2.FONT_HERSHEY_SIMPLEX, 0.45, f_col, 1)
    cv2.putText(frame, f"Servo  : {hardware_state['SERVO_ANGLE']} deg", (20, 95), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 200, 255), 1)

    # --- TELEMETRY HUD (SOFTWARE TRIGGER) ---
    h, w, _ = frame.shape
    x_start = w - 290
    cv2.rectangle(frame, (x_start, 10), (w - 10, 150), (20, 20, 20), -1)
    meta = software_actions.get(current_locked_gesture, software_actions["None"])
    cv2.rectangle(frame, (x_start, 10), (w - 10, 150), meta["color"], 2)
    cv2.putText(frame, "SOFTWARE TRIGGER", (x_start + 15, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1)
    cv2.putText(frame, meta["action"], (x_start + 15, 135), cv2.FONT_HERSHEY_SIMPLEX, 0.48, meta["color"], 2)

    # Instant blit of pre-rendered emoji
    emoji_patch = emoji_cache.get(current_locked_gesture, emoji_cache["None"])
    frame[45:115, x_start + 105: x_start + 175] = emoji_patch

    cv2.imshow("Hardware + Software Multi-Modal Controller", frame)
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

stream.release()
cv2.destroyAllWindows()