/*
  Wizards - Cauldron ESP32 Controller
  =======================================
  
  This ESP32 controls the cauldron input and sends serial commands
  to a sprite player when the cauldron is solved. When triggered by
  grounding the input, it plays a video sequence and notifies the
  central controller to unlock the dream runes. Can only be solved
  once until system reset.
  
  Hardware:
  - ESP32 development board
  - Input sensor (button, proximity, etc.)
  - Serial connection to sprite player (TX/RX pins)
  - Pull-up resistor (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/gamestate/cauldron_solved
  
  Author: Wizards Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

// WiFi credentials
const char* ssid = "Verizon_V3N3DV";
const char* password = "tarry4says9nick";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "cauldron_esp32";

// MQTT topics
const char* topic_cauldron_solved = "escaperoom/esp32/cauldron/pressed";
const char* topic_reset = "escaperoom/system/reset";

// Hardware pins
const int SENSOR_PIN = 8;    // Cauldron sensor input pin
const int SERIAL_TX = 6;    // Serial TX to sprite player
const int SERIAL_RX = 7;    // Serial RX from sprite player

// Serial communication
HardwareSerial SpriteSerial(1);  // Use hardware serial port 1

// Video file commands (single bytes - NUMERICAL values, not characters)
const byte VIDEO_CAULDRON_SEQUENCE = 1;  // Cauldron sequence video

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_sensor_state = HIGH;
bool current_sensor_state = HIGH;
bool cauldron_was_triggered = false;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// Sprite player communication
unsigned long last_sprite_check = 0;
const unsigned long SPRITE_CHECK_INTERVAL = 100; // Check sprite status every 100ms


void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Cauldron ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  
  // Initialize sprite player serial communication
  SpriteSerial.begin(9600, SERIAL_8N1, SERIAL_RX, SERIAL_TX);
  delay(500);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Waiting for cauldron interaction...");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Read and process sensor input
  process_sensor_input();
  
  // Handle sprite player communication
  handle_sprite_communication();
  
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
      
      // Subscribe to reset topic
      mqtt_client.subscribe(topic_reset);
      Serial.print("Subscribed to reset topic: ");
      Serial.println(topic_reset);
      
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
  
  // Handle reset command
  if (String(topic) == topic_reset) {
    if (message == "reset" || message == "true") {
      reset_system();
    }
  }
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
      
      // Check for sensor activation (input grounded = cauldron solved)
      if (current_sensor_state == LOW && !cauldron_was_triggered) {
        cauldron_triggered();
        cauldron_was_triggered = true;  // Only solve once until reset
      }
    }
  }
  
  last_sensor_state = reading;
}

void cauldron_triggered() {
  Serial.println("=== CAULDRON SOLVED! ===");
  
  // Send command to sprite player to trigger video
  play_video(VIDEO_CAULDRON_SEQUENCE);
  
  if (mqtt_client.connected()) {
    // Publish cauldron solved event (tells central controller to unlock dream runes)
    mqtt_client.publish(topic_cauldron_solved, "true");
    Serial.println("Published: Cauldron solved - dream runes will be unlocked!");

    Serial.println("Cauldron permanently solved until system reset");
    
  } else {
    Serial.println("MQTT not connected - message not sent!");
  }
}

void play_video(byte video_number) {
  Serial.print("Sending sprite command for video ");
  Serial.print(video_number, DEC);
  Serial.print(" (0x");
  Serial.print(video_number, HEX);
  Serial.println(")");
  
  // Send single byte command to sprite player
  SpriteSerial.write(video_number);
  
  // Brief delay to allow command processing
  delay(50);
}

void reset_system() {
  Serial.println("=== SYSTEM RESET TRIGGERED ===");
  Serial.println("Resetting cauldron to original state...");
  
  // Reset cauldron state
  cauldron_was_triggered = false;
  last_debounce_time = 0;
  last_sensor_state = HIGH;
  current_sensor_state = HIGH;
  
  Serial.println("Cauldron reset complete - ready for activation");
  
  // Publish reset confirmation
  if (mqtt_client.connected()) {
    String status_topic = String("escaperoom/status/") + device_id;
    mqtt_client.publish(status_topic.c_str(), "reset_complete");
  }
}

void handle_sprite_communication() {
  // Check for sprite player responses periodically
  unsigned long current_time = millis();
  
  if (current_time - last_sprite_check >= SPRITE_CHECK_INTERVAL) {
    if (SpriteSerial.available()) {
      while (SpriteSerial.available()) {
        byte response = SpriteSerial.read();
        
        if (response == 0xEE) {
          Serial.println("Sprite player: End of file reached");
        } else {
          Serial.print("Sprite player status: Playing file ");
          Serial.print(response, DEC);
          Serial.print(" (0x");
          Serial.print(response, HEX);
          Serial.println(")");
        }
      }
    }
    last_sprite_check = current_time;
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
                             ",\"cauldron_solved\":" + (cauldron_was_triggered ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}