/*
  Escape Room - Cheese ESP32 Controller
  =====================================
  
  This ESP32 controls the cheese input sensor.
  When activated, it triggers cheese audio playback
  and unlocks the rat trap door maglock.
  
  Hardware:
  - ESP32 development board
  - Pressure sensor, button, or proximity sensor
  - Pull-up resistor (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/cheese/pressed
  - Publishes: escaperoom/audio/cheese (for audio request)
  
  Author: Escape Room Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "EscapeRoom_WiFi";
const char* password = "YourWiFiPassword";

// MQTT settings
const char* mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;
const char* device_id = "cheese_esp32";

// MQTT topics
const char* topic_cheese_pressed = "escaperoom/esp32/cheese/pressed";
const char* topic_cheese_audio = "escaperoom/audio/cheese";

// Hardware pins
const int SENSOR_PIN = 2;  // Cheese sensor input pin

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_sensor_state = HIGH;
bool current_sensor_state = HIGH;
bool cheese_was_triggered = false;

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
  
  Serial.println("=== Cheese ESP32 Controller ===");
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
  Serial.println("Waiting for cheese interaction...");
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
    // Continue anyway - might connect later
  }
}

void reconnect_mqtt() {
  while (!mqtt_client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    if (mqtt_client.connect(device_id)) {
      Serial.println(" connected");
      
      // Publish online status
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
  // Convert payload to string
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Received MQTT message [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  // Handle any incoming messages if needed
  // This device mainly publishes, doesn't usually receive
}

void process_sensor_input() {
  // Read the sensor (LOW when triggered with pull-up)
  bool reading = digitalRead(SENSOR_PIN);
  
  // Handle debouncing
  if (reading != last_sensor_state) {
    last_debounce_time = millis();
  }
  
  if ((millis() - last_debounce_time) > DEBOUNCE_DELAY) {
    if (reading != current_sensor_state) {
      current_sensor_state = reading;
      
      // Check for sensor activation (LOW = triggered with pull-up)
      if (current_sensor_state == LOW && !cheese_was_triggered) {
        cheese_triggered();
        cheese_was_triggered = true;  // Only trigger once
      }
      
      // Reset flag when sensor is released (if it's a momentary sensor)
      if (current_sensor_state == HIGH) {
        // For some sensors, you might want to reset immediately
        // For others (like pressure plates), you might want a delay
        reset_trigger_after_delay();
      }
    }
  }
  
  last_sensor_state = reading;
}

void cheese_triggered() {
  Serial.println("=== CHEESE TRIGGERED! ===");
  
  if (mqtt_client.connected()) {
    // Publish cheese pressed event (unlocks rat trap door maglock)
    mqtt_client.publish(topic_cheese_pressed, "true");
    Serial.println("Published: Cheese pressed - rat trap door will unlock");
    
    // Publish audio request
    mqtt_client.publish(topic_cheese_audio, "play");
    Serial.println("Published: Cheese audio request");
    
    // Flash LED rapidly to indicate success
    for (int i = 0; i < 8; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
    }
    
    // Keep LED on for a few seconds to show completion
    digitalWrite(LED_BUILTIN, HIGH);
    delay(2000);
    digitalWrite(LED_BUILTIN, LOW);
    
  } else {
    Serial.println("MQTT not connected - messages not sent!");
  }
}

void reset_trigger_after_delay() {
  // Reset the trigger flag after a delay (for re-triggering if needed)
  // This could be immediate or after a timeout depending on your sensor type
  static unsigned long last_reset_time = 0;
  unsigned long current_time = millis();
  
  // Reset after 5 seconds (adjust as needed for your sensor type)
  if (current_time - last_reset_time >= 5000) {
    cheese_was_triggered = false;
    last_reset_time = current_time;
  }
}

void handle_status_led() {
  // Blink status LED to show system is running
  // Skip blinking if we just triggered (LED might be on for success indication)
  if (!cheese_was_triggered) {
    unsigned long current_time = millis();
    
    if (current_time - last_blink_time >= BLINK_INTERVAL) {
      led_blink_state = !led_blink_state;
      digitalWrite(LED_BUILTIN, led_blink_state);
      last_blink_time = current_time;
    }
  }
}

void publish_heartbeat() {
  // Publish heartbeat every 30 seconds
  static unsigned long last_heartbeat = 0;
  unsigned long current_time = millis();
  
  if (current_time - last_heartbeat >= 30000) {
    if (mqtt_client.connected()) {
      String heartbeat_topic = String("escaperoom/heartbeat/") + device_id;
      String heartbeat_data = String("{\"uptime\":") + (current_time / 1000) + 
                             ",\"free_heap\":" + ESP.getFreeHeap() + 
                             ",\"triggered\":" + (cheese_was_triggered ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}