/*
  Escape Room - Cross/Purple Crystal ESP32 Controller
  ==================================================
  
  This ESP32 controls the cross button input.
  When pressed, it publishes an MQTT message that unlocks
  the purple crystal compartment maglock.
  
  Hardware:
  - ESP32 development board
  - Momentary push button
  - Pull-up resistor (or use internal pull-up)
  
  MQTT Topics:
  - Publishes: escaperoom/esp32/cross/pressed
  
  Author: Escape Room Control System
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// WiFi credentials
const char* ssid = "EscapeRoom_WiFi";
const char* password = "YourWiFiPassword";

// MQTT settings
const char* mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;
const char* device_id = "cross_esp32";

// Audio server settings
const char* audio_server_ip = "192.168.1.150";
const int audio_server_port = 80;

// MQTT topics
const char* topic_cross_pressed = "escaperoom/esp32/cross/pressed";

// Hardware pins
const int BUTTON_PIN = 2;  // Cross button input pin

// Debounce settings
const unsigned long DEBOUNCE_DELAY = 50;  // milliseconds
unsigned long last_debounce_time = 0;
bool last_button_state = HIGH;
bool current_button_state = HIGH;
bool button_was_pressed = false;

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
  
  Serial.println("=== Cross/Purple Crystal ESP32 Controller ===");
  Serial.println("Initializing...");
  
  // Initialize pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup MQTT
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqtt_callback);
  
  Serial.println("Initialization complete");
  Serial.println("Waiting for cross button press...");
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    reconnect_mqtt();
  }
  mqtt_client.loop();
  
  // Handle status LED blinking
  handle_status_led();
  
  // Read and process button input
  process_button_input();
  
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

void process_button_input() {
  // Read the button (LOW when pressed with pull-up)
  bool reading = digitalRead(BUTTON_PIN);
  
  // Handle debouncing
  if (reading != last_button_state) {
    last_debounce_time = millis();
  }
  
  if ((millis() - last_debounce_time) > DEBOUNCE_DELAY) {
    if (reading != current_button_state) {
      current_button_state = reading;
      
      // Check for button press (LOW = pressed with pull-up)
      if (current_button_state == LOW && !button_was_pressed) {
        button_pressed();
        button_was_pressed = true;  // Only trigger once
      }
      
      // Reset flag when button is released
      if (current_button_state == HIGH) {
        button_was_pressed = false;
      }
    }
  }
  
  last_button_state = reading;
}

void button_pressed() {
  Serial.println("=== CROSS BUTTON PRESSED! ===");
  
  // Trigger cross/purple crystal sound
  trigger_audio_event("cross_placed_sound");
  
  // Publish MQTT message
  if (mqtt_client.connected()) {
    mqtt_client.publish(topic_cross_pressed, "true");
    Serial.println("Published: Cross button pressed");
    
    // Flash LED rapidly to indicate success
    for (int i = 0; i < 6; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
    }
  } else {
    Serial.println("MQTT not connected - message not sent!");
  }
}

void trigger_audio_event(String event_name) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + String(audio_server_ip) + "/" + event_name;
    http.begin(url);
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
      Serial.println("Audio event triggered: " + event_name);
    } else {
      Serial.println("Audio request failed: " + String(httpResponseCode));
    }
    
    http.end();
  } else {
    Serial.println("WiFi not connected - cannot trigger audio");
  }
}

void handle_status_led() {
  // Blink status LED to show system is running
  unsigned long current_time = millis();
  
  if (current_time - last_blink_time >= BLINK_INTERVAL) {
    led_blink_state = !led_blink_state;
    digitalWrite(LED_BUILTIN, led_blink_state);
    last_blink_time = current_time;
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
                             ",\"button_pressed_count\":" + "1" + "}";
      
      mqtt_client.publish(heartbeat_topic.c_str(), heartbeat_data.c_str());
    }
    last_heartbeat = current_time;
  }
}