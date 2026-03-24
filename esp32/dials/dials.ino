/*
  Escape Room - Dials ESP32 Controller
  ====================================
  
  This ESP32 controls the treasure chest dials puzzle.
  When the dials are set to the correct combination,
  it publishes an MQTT message to unlock the treasure chest maglock.
  
  Hardware:
  - ESP32 development board
  - Dial/combination sensor (button, switch, or completion detector)
  - Pull-up resistor (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/dials/solved
  
  Author: Escape Room Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "dials_esp32";

// Audio server settings  
const char* audio_server_ip = "192.168.1.156";
const int audio_server_port = 15000;

// MQTT topics
const char* topic_dials_solved = "escaperoom/esp32/dials/solved";
const char* topic_reset = "escaperoom/system/reset";

// Hardware pins
const int SENSOR_PIN = 6;  // Dials completion sensor input pin

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_sensor_state = HIGH;
bool current_sensor_state = HIGH;
bool dials_were_solved = false;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Dials ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Waiting for dials to be solved...");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();

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
      
      // Subscribe to reset topic
      mqtt_client.subscribe(topic_reset);
      Serial.print("Subscribed to reset topic: ");
      Serial.println(topic_reset);
      
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
  
  // Handle reset command
  if (String(topic) == topic_reset) {
    if (message == "reset" || message == "true") {
      reset_system();
    }
  }
}

void process_sensor_input() {
  // Read the sensor (LOW when dials solved with pull-up)
  bool reading = digitalRead(SENSOR_PIN);
  
  // Handle debouncing
  if (reading != last_sensor_state) {
    last_debounce_time = millis();
  }
  
  if ((millis() - last_debounce_time) > DEBOUNCE_DELAY) {
    if (reading != current_sensor_state) {
      current_sensor_state = reading;
      
      // Check for dials completion (LOW = solved with pull-up)
      if (current_sensor_state == LOW && !dials_were_solved) {
        dials_solved();
        dials_were_solved = true;  // Only trigger once
      }
    }
  }
  
  last_sensor_state = reading;
}

void dials_solved() {
  Serial.println("=== DIALS PUZZLE SOLVED! ===");
  Serial.println("Treasure chest will unlock!");
  
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_dials_solved, "true");
    Serial.println("Published: Dials solved - treasure chest unlocked");
    
  } else {
    Serial.println("MQTT not connected - message not sent!");
  }
}

void reset_system() {
  Serial.println("=== SYSTEM RESET TRIGGERED ===");
  Serial.println("Resetting dials puzzle to original state...");
  
  // Reset dials state
  dials_were_solved = false;
  last_debounce_time = 0;
  last_sensor_state = HIGH;
  current_sensor_state = HIGH;
  
  Serial.println("Dials reset complete - ready for solving");
  
  // Publish reset confirmation
  if (mqtt_client.connected()) {
    String status_topic = String("escaperoom/status/") + device_id;
    mqtt_client.publish(status_topic.c_str(), "reset_complete");
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
                             ",\"dials_solved\":" + (dials_were_solved ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}