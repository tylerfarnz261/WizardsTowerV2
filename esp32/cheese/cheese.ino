/*
  Escape Room - Combined Cheese ESP32 Controller
  ===================================================================
  
  This ESP32 controls:
  1. Cheese input sensor - triggers cheese audio playback and unlocks rat trap door maglock
  
  Both sensors can only activate once until system is reset via MQTT.
  
  Hardware:
  - ESP32 development board
  - Cheese sensor on pin 6
  - Watcher paintings sensor on pin 7
  - Pull-up resistors (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/cheese/pressed
  - Publishes: escaperoom/audio/cheese
  - Publishes: escaperoom/esp32/watcher/pressed
  - Subscribes: escaperoom/esp32/combined/reset
  
  Author: Wizards Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "wizardscontroller.local";
const int mqtt_port = 1883;
const char* device_id = "cheese_esp32";

// MQTT topics
const char* topic_cheese_pressed = "escaperoom/esp32/cheese/pressed";
const char* topic_reset = "escaperoom/system/reset";

// Hardware pins
const int CHEESE_SENSOR_PIN = 6;    // Cheese sensor input pin

// Debounce settings for both sensors
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds

// Cheese sensor variables
unsigned long cheese_last_debounce_time = 0;
bool cheese_last_sensor_state = HIGH;
bool cheese_current_sensor_state = HIGH;
bool cheese_was_triggered = false;
bool cheese_solved = false;  // Track if solved via MQTT

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Cheese ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(CHEESE_SENSOR_PIN, INPUT_PULLUP);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Waiting for cheese interaction...");
  Serial.println("Cheese sensor on pin 6");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Read and process cheese sensor input
  process_cheese_sensor_input();
  
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
      
      // Subscribe to reset topic
      mqtt_client.subscribe(topic_reset);
      Serial.print("Subscribed to reset topic: ");
      Serial.println(topic_reset);
      
      // Subscribe to own topics to prevent re-triggering after manual activation
      mqtt_client.subscribe(topic_cheese_pressed);
      Serial.println("Subscribed to self-publishing topic");
      
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
  
  // Handle reset command
  if (String(topic) == topic_reset) {
    if (message == "reset" || message == "true") {
      reset_system();
    }
  }
  // Handle cheese solved (prevents re-triggering)
  else if (String(topic) == topic_cheese_pressed && message.equalsIgnoreCase("true")) {
    Serial.println("Cheese marked as solved - preventing physical re-trigger");
    cheese_solved = true;
  }
}

void process_cheese_sensor_input() {
  // Read the cheese sensor (LOW when triggered with pull-up)
  bool reading = digitalRead(CHEESE_SENSOR_PIN);
  
  // Handle debouncing
  if (reading != cheese_last_sensor_state) {
    cheese_last_debounce_time = millis();
  }
  
  if ((millis() - cheese_last_debounce_time) > DEBOUNCE_DELAY) {
    if (reading != cheese_current_sensor_state) {
      cheese_current_sensor_state = reading;
      
      // Check for sensor activation (LOW = triggered with pull-up)
      if (cheese_current_sensor_state == LOW && !cheese_was_triggered) {
        // Check if already solved (prevents re-triggering)
        if (cheese_solved) {
          Serial.println("Cheese already solved - ignoring physical trigger");
        } else {
          cheese_triggered();
          cheese_was_triggered = true;  // Only trigger once until reset
        }
      }
    }
  }
  
  cheese_last_sensor_state = reading;
}

void cheese_triggered() {
  Serial.println("=== CHEESE TRIGGERED! ===");
  
  if (mqtt_client.connected()) {
    // Publish cheese pressed event (unlocks rat trap door maglock)
    mqtt_client.publish(topic_cheese_pressed, "true");
    Serial.println("Published: Cheese pressed - rat trap door will unlock");
    
  } else {
    Serial.println("MQTT not connected - messages not sent!");
  }
}

void reset_system() {
  Serial.println("=== SYSTEM RESET TRIGGERED ===");
  Serial.println("Resetting cheese sensor to original state...");
  
  // Reset cheese sensor state
  cheese_was_triggered = false;
  cheese_solved = false;  // Reset solved flag
  cheese_last_debounce_time = 0;
  cheese_last_sensor_state = HIGH;
  cheese_current_sensor_state = HIGH;
  
  Serial.println("System reset complete - cheese sensor ready for activation");
  
  // Publish reset confirmation
  if (mqtt_client.connected()) {
    String status_topic = String("escaperoom/status/") + device_id;
    mqtt_client.publish(status_topic.c_str(), "reset_complete");
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
                             ",\"cheese_triggered\":" + (cheese_was_triggered ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}