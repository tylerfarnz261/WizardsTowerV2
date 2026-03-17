/*
  Escape Room - Watcher Paintings ESP32 Controller
  ================================================
  
  This ESP32 controls the four watcher paintings input.
  When the paintings are aligned or the mechanism is triggered,
  it publishes an MQTT message to unlock a maglock.
  
  Hardware:
  - ESP32 development board
  - Painting alignment sensor or button
  - Pull-up resistor (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/watcher/pressed
  
  Author: Escape Room Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "EscapeRoom_WiFi";
const char* password = "YourWiFiPassword";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "watcher_paintings_esp32";

// MQTT topics
const char* topic_watcher_triggered = "escaperoom/esp32/watcher/pressed";

// Hardware pins
const int SENSOR_PIN = 2;  // Watcher paintings sensor input pin

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_sensor_state = HIGH;
bool current_sensor_state = HIGH;
bool watcher_was_triggered = false;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// Status LED (built-in)
bool led_blink_state = false;
unsigned long last_blink_time = 0;
const unsigned long BLINK_INTERVAL = 1000;  // 1 second

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Watcher Paintings ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Waiting for watcher paintings to be aligned...");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Handle status LED blinking
  handle_status_led();
  
  // Read and process sensor input
  process_sensor_input();
  
  // Publish heartbeat periodically
  publish_heartbeat();
  
  delay(10);  // Small delay for stability
}

void setup_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed!");
  }
}

void reconnect_mqtt() {
  while (!mqtt_client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    if (mqtt_client.connect(device_id)) {
      Serial.println(" connected");
      
      String status_topic = String("escaperoom/status/") + device_id;
      mqtt_client.publish(status_topic.c_str(), "online");
      
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Received MQTT message [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
}

void process_sensor_input() {
  // Read the sensor (LOW when paintings aligned with pull-up)
  bool reading = digitalRead(SENSOR_PIN);
  
  // Handle debouncing
  if (reading != last_sensor_state) {
    last_debounce_time = millis();
  }
  
  if ((millis() - last_debounce_time) > DEBOUNCE_DELAY) {
    if (reading != current_sensor_state) {
      current_sensor_state = reading;
      
      // Check for paintings alignment (LOW = aligned with pull-up)
      if (current_sensor_state == LOW && !watcher_was_triggered) {
        watcher_triggered();
        watcher_was_triggered = true;  // Only trigger once
      }
      
      // Allow re-triggering if paintings go out of alignment and back
      if (current_sensor_state == HIGH) {
        reset_after_delay();
      }
    }
  }
  
  last_sensor_state = reading;
}

void watcher_triggered() {
  Serial.println("=== WATCHER PAINTINGS ALIGNED! ===");
  Serial.println("Maglock will unlock!");
  
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_watcher_triggered, "true");
    Serial.println("Published: Watcher paintings triggered - maglock unlocked");
    
    // Success light sequence
    for (int i = 0; i < 6; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(125);
      digitalWrite(LED_BUILTIN, LOW);
      delay(125);
    }
    
    // Keep LED on briefly to show completion
    digitalWrite(LED_BUILTIN, HIGH);
    delay(2000);
    digitalWrite(LED_BUILTIN, LOW);
    
  } else {
    Serial.println("MQTT not connected - message not sent!");
  }
}

void reset_after_delay() {
  // Allow re-triggering after paintings are moved and aligned again
  static unsigned long last_reset_time = 0;
  unsigned long current_time = millis();
  
  // Reset after 3 seconds (adjust as needed)
  if (current_time - last_reset_time >= 3000) {
    watcher_was_triggered = false;
    last_reset_time = current_time;
    Serial.println("Watcher trigger reset - can be activated again");
  }
}

void handle_status_led() {
  // Blink status LED to show system is running
  if (!watcher_was_triggered) {
    unsigned long current_time = millis();
    
    if (current_time - last_blink_time >= BLINK_INTERVAL) {
      led_blink_state = !led_blink_state;
      digitalWrite(LED_BUILTIN, led_blink_state);
      last_blink_time = current_time;
    }
  }
}

void publish_heartbeat() {
  static unsigned long last_heartbeat = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_heartbeat >= 30000) {
    if (mqtt_client.connected()) {
      String heartbeat_topic = String("escaperoom/heartbeat/") + device_id;
      String heartbeat_data = String("{\"uptime\":") + (current_time / 1000) + 
                             ",\"free_heap\":" + ESP.getFreeHeap() + 
                             ",\"triggered\":" + (watcher_was_triggered ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}