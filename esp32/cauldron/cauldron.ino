/*
  Wizards - Cauldron ESP32 Controller
  =======================================
  
  This ESP32 controls the cauldron input and sends serial commands
  to a sprite player. When triggered, it also marks the cauldron
  as solved, which unlocks the dream runes.
  
  Hardware:
  - ESP32 development board
  - Input sensor (button, proximity, etc.)
  - Serial connection to sprite player (TX/RX pins)
  - Pull-up resistor (if not using internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/cauldron/pressed
  - Publishes: escaperoom/gamestate/cauldron_solved
  
  Author: Wizards Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

// WiFi credentials
const char* ssid = "EscapeRoom_WiFi";
const char* password = "YourWiFiPassword";

// MQTT settings
const char* mqtt_server = "192.168.0.194";
const int mqtt_port = 1883;
const char* device_id = "cauldron_esp32";

// MQTT topics
const char* topic_cauldron_pressed = "escaperoom/esp32/cauldron/pressed";
const char* topic_cauldron_solved = "escaperoom/gamestate/cauldron_solved";

// Hardware pins
const int SENSOR_PIN = 2;    // Cauldron sensor input pin
const int SERIAL_TX = 17;    // Serial TX to sprite player
const int SERIAL_RX = 16;    // Serial RX from sprite player

// Serial communication
HardwareSerial SpriteSerial(1);  // Use hardware serial port 1

// Sprite player commands
const String SPRITE_COMMAND_CAULDRON = "PLAY_CAULDRON_SEQUENCE\n";
const String SPRITE_COMMAND_STOP = "STOP\n";

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_sensor_state = HIGH;
bool current_sensor_state = HIGH;
bool cauldron_was_triggered = false;

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
  
  Serial.println("=== Cauldron ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Initialize sprite player serial communication
  SpriteSerial.begin(9600, SERIAL_8N1, SERIAL_RX, SERIAL_TX);
  delay(100);
  
  // Send a test command to sprite player
  SpriteSerial.print("HELLO\n");
  Serial.println("Sent HELLO command to sprite player");
  
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
  
  // Handle status LED blinking
  handle_status_led();
  
  // Read and process sensor input
  process_sensor_input();
  
  // Handle sprite player serial communication
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
  // Could handle remote sprite player commands here
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
      if (current_sensor_state == LOW && !cauldron_was_triggered) {
        cauldron_triggered();
        cauldron_was_triggered = true;  // Only trigger once
      }
    }
  }
  
  last_sensor_state = reading;
}

void cauldron_triggered() {
  Serial.println("=== CAULDRON TRIGGERED! ===");
  
  // Send command to sprite player
  send_sprite_command(SPRITE_COMMAND_CAULDRON);
  
  if (mqtt_client.connected()) {
    // Publish cauldron pressed event
    mqtt_client.publish(topic_cauldron_pressed, "true");
    Serial.println("Published: Cauldron pressed");
    
    // Publish cauldron solved event (unlocks dream runes)
    mqtt_client.publish(topic_cauldron_solved, "true");
    Serial.println("Published: Cauldron solved - dream runes unlocked!");
    
    // Flash LED rapidly to indicate success
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
    }
    
    // Keep LED on for a few seconds to show completion
    digitalWrite(LED_BUILTIN, HIGH);
    delay(3000);
    digitalWrite(LED_BUILTIN, LOW);
    
  } else {
    Serial.println("MQTT not connected - messages not sent!");
  }
}

void send_sprite_command(String command) {
  Serial.print("Sending sprite command: ");
  Serial.print(command);
  
  SpriteSerial.print(command);
  
  // Wait a moment for response
  delay(100);
  
  // Check for acknowledgment
  if (SpriteSerial.available()) {
    String response = SpriteSerial.readString();
    Serial.print("Sprite player response: ");
    Serial.println(response);
  } else {
    Serial.println("No response from sprite player");
  }
}

void handle_sprite_communication() {
  // Handle any incoming data from sprite player
  if (SpriteSerial.available()) {
    String message = SpriteSerial.readString();
    Serial.print("Sprite player says: ");
    Serial.println(message);
    
    // Could process status messages, completion confirmations, etc.
    if (message.indexOf("SEQUENCE_COMPLETE") != -1) {
      Serial.println("Sprite sequence completed successfully!");
    }
  }
}

void handle_status_led() {
  // Blink status LED to show system is running
  // Skip blinking if we just triggered (LED might be on for success indication)
  if (!cauldron_was_triggered) {
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
                             ",\"cauldron_solved\":" + (cauldron_was_triggered ? "true" : "false") + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}