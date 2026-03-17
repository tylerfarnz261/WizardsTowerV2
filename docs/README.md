# Escape Room Control System
## Complete Setup and Installation Guide

This escape room control system consists of multiple ESP32 devices, two Raspberry Pi controllers, and an MQTT messaging system to create an immersive puzzle experience.

## System Overview

### Architecture
- **2 Raspberry Pi Controllers**:
  - **Rune Controller Pi**: Manages rune puzzles with MCP23017 GPIO expanders
  - **Central Controller Pi**: Controls maglocks and hosts MQTT broker
- **8 ESP32 Devices**: Handle individual puzzle inputs
- **MQTT Network**: Coordinates communication between all devices
- **Windows Audio System**: Handles audio playback requests

### Communication Flow
```
ESP32 Devices → MQTT → Central Controller → Maglocks
ESP32 Devices → MQTT → Rune Controller → Audio/Lighting
Rune Controller → HTTP → Spell Success → MQTT Actions
```

## Hardware Requirements

### Raspberry Pi Controllers (x2)

#### Rune Controller Pi
- Raspberry Pi 4 (recommended) or Pi 3B+
- MicroSD card (32GB+, Class 10)
- 2x MCP23017 16-bit GPIO expanders
- 12 momentary push buttons (runes)
- 12 LEDs with current limiting resistors
- Breadboard and jumper wires
- Power supply (5V, 3A)

#### Central Controller Pi  
- Raspberry Pi 4 (recommended) or Pi 3B+
- MicroSD card (32GB+, Class 10) 
- 9 relay modules (for maglocks)
- Power supply (5V, 3A)
- Ethernet connection for MQTT broker stability

### ESP32 Devices (x8)

Each ESP32 requires:
- ESP32 development board (ESP32-DevKitC or similar)
- MicroUSB cable for programming
- Sensors/inputs specific to puzzle (see individual sections)
- Breadboard and jumper wires
- Power supply or USB power

### Additional Hardware
- MQTT-compatible router/network
- Windows PC for audio system (optional)
- 9 electromagnetic maglocks
- Various sensors, buttons, and puzzle-specific hardware

## Software Installation

### Raspberry Pi Setup (Both Controllers)

#### 1. Operating System Installation
```bash
# Flash Raspberry Pi OS Lite to SD card using Raspberry Pi Imager
# Enable SSH during imaging or add ssh file to boot partition
# Boot Pi and connect via SSH

sudo apt update && sudo apt upgrade -y
```

#### 2. Python Environment Setup
```bash
# Install Python dependencies
sudo apt install python3-pip python3-venv git -y

# Clone project (or copy files)
git clone <your-repo-url> /home/pi/wizards
cd /home/pi/wizards

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install Python packages
pip install -r requirements.txt
```

#### 3. I2C and GPIO Configuration (Rune Controller Only)
```bash
# Enable I2C interface
sudo raspi-config
# Navigate to: Interface Options → I2C → Yes

# Verify I2C is working
sudo i2cdetect -y 1
# Should show addresses 0x20 and 0x21 when MCP23017s are connected
```

#### 4. MQTT Broker Setup (Central Controller Only)
```bash
# Install Mosquitto MQTT broker
sudo apt install mosquitto mosquitto-clients -y

# Copy configuration
sudo cp /home/pi/wizards/config/mosquitto.conf /etc/mosquitto/conf.d/wizards.conf

# Enable and start Mosquitto
sudo systemctl enable mosquitto
sudo systemctl start mosquitto

# Test MQTT broker
mosquitto_pub -h localhost -t test/topic -m "Hello MQTT"
mosquitto_sub -h localhost -t test/topic
```

#### 5. Service Installation

**Rune Controller Service:**
```bash
# Copy service file
sudo cp /home/pi/wizards/raspberry_pi/rune_controller/rune-controller.service /etc/systemd/system/

# Make startup script executable
chmod +x /home/pi/wizards/raspberry_pi/rune_controller/start_rune_controller.sh

# Enable and start service
sudo systemctl enable rune-controller
sudo systemctl start rune-controller

# Check status
sudo systemctl status rune-controller
```

**Central Controller Service:**
```bash
# Copy service file
sudo cp /home/pi/wizards/raspberry_pi/central_controller/central-controller.service /etc/systemd/system/

# Make startup script executable  
chmod +x /home/pi/wizards/raspberry_pi/central_controller/start_central_controller.sh

# Enable and start service
sudo systemctl enable central-controller
sudo systemctl start central-controller

# Check status
sudo systemctl status central-controller
```

### ESP32 Programming

#### 1. Arduino IDE Setup
1. Install Arduino IDE 2.0+ 
2. Add ESP32 board support:
   - File → Preferences
   - Additional Boards Manager URLs: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board Manager → Search "ESP32" → Install

#### 2. Required Libraries
Install these libraries via Library Manager:
- WiFi (built-in)
- PubSubClient (by Nick O'Leary)
- Adafruit NeoPixel (for staircase ESP32)

#### 3. Programming Each ESP32
1. Update WiFi credentials in each .ino file
2. Update MQTT broker IP address
3. Connect ESP32 via USB
4. Select board: ESP32 Dev Module
5. Upload code to each ESP32
6. Test functionality using Serial Monitor

## Network Configuration

### IP Address Assignment
Update these IPs in `config/system_config.yaml`:
```yaml
network:
  rune_controller_ip: "192.168.0.193"
  central_controller_ip: "192.168.0.194" 
  mqtt_broker_ip: "192.168.0.194"      # Same as one Pi
  windows_audio_ip: "192.168.1.150"
```

### MQTT Topics
All MQTT topics are defined in `config/mqtt_topics.yaml`. Key topics:
- `escaperoom/esp32/*/` - ESP32 device outputs
- `escaperoom/runes/*/` - Rune activations  
- `escaperoom/maglocks/*/` - Maglock control
- `escaperoom/system/*/` - System control

## Hardware Wiring

### Rune Controller Pi Wiring

#### MCP23017 Connections
```
Pi GPIO → MCP23017 #1 (0x20)       Pi GPIO → MCP23017 #2 (0x21)
SDA     → SDA (Pin 13)             SDA     → SDA (Pin 13) 
SCL     → SCL (Pin 12)             SCL     → SCL (Pin 12)
3.3V    → VDD (Pin 9)              3.3V    → VDD (Pin 9)
GND     → VSS (Pin 10)             GND     → VSS (Pin 10)
3.3V    → RESET (Pin 18)           3.3V    → RESET (Pin 18)
A0,A1,A2 → GND (Address 0x20)      A0 → 3.3V, A1,A2 → GND (0x21)
```

#### Rune Connections (Example - Fire Torch 1)
```
MCP23017 #1:
Pin 21 (GPA0) → Button → GND (with pull-up enabled)
Pin 22 (GPA1) → LED + → (LED - to GND via resistor)
```

### Central Controller Pi Wiring

#### Maglock Relay Connections
```
Pi GPIO → Relay Module → Maglock
GPIO 2  → Relay 1      → Paradox Compartment
GPIO 3  → Relay 2      → Wand Cabinet
GPIO 4  → Relay 3      → Purple Crystal Compartment
...etc (see system_config.yaml)
```

### ESP32 Wiring Examples

#### Simple Button ESP32 (Wand Cabinet, Cross, etc.)
```
ESP32 Pin 2 → Button → GND
(Internal pull-up enabled in code)
```

#### Staircase ESP32
```
ESP32 Pin 2,4,5,18,19 → Buttons → GND
ESP32 Pin 13 → NeoPixel Data In
5V → NeoPixel VCC
GND → NeoPixel GND
```

## Testing and Troubleshooting

### System Health Checks

#### 1. MQTT Broker Test
```bash
# On Central Controller Pi
mosquitto_sub -h localhost -t escaperoom/+/+

# On another terminal
mosquitto_pub -h localhost -t escaperoom/test -m "hello"
```

#### 2. Service Status
```bash
# Check all services
sudo systemctl status central-controller
sudo systemctl status rune-controller  
sudo systemctl status mosquitto

# View logs
journalctl -u central-controller -f
journalctl -u rune-controller -f
```

#### 3. Flask Endpoints Test
```bash
# Test Rune Controller
curl http://192.168.0.193:5001/status

# Test Central Controller  
curl http://192.168.0.194:5002/status
curl http://192.168.0.194:5002/maglocks
```

#### 4. ESP32 Diagnostics
- Check Serial Monitor for connection status
- Verify WiFi connection
- Monitor MQTT publish/subscribe activity
- Check heartbeat messages

### Common Issues and Solutions

#### MQTT Connection Issues
- Verify broker IP addresses in all configs
- Check firewall settings on Pi
- Ensure ESP32s can reach MQTT broker
- Check mosquitto service status

#### GPIO/I2C Issues
- Verify I2C is enabled: `sudo raspi-config`
- Check wiring with multimeter
- Test I2C devices: `sudo i2cdetect -y 1`
- Check for loose connections

#### Flask Service Issues
- Check service logs: `journalctl -u service-name -f`
- Verify Python environment and dependencies
- Check port conflicts (5001, 5002)
- Test manual startup outside systemd

#### ESP32 Issues  
- Check WiFi credentials and signal strength
- Verify MQTT broker accessibility
- Update board definitions and libraries
- Check power supply stability

### Game Logic Testing

#### Manual Puzzle Solving
Use the Flask endpoints to manually trigger puzzles:
```bash
# Unlock specific maglock
curl -X POST http://192.168.0.194:5002/maglocks/wand_cabinet/unlock

# Simulate puzzle solve
curl -X POST http://192.168.0.194:5002/puzzle/cross/solve

# Trigger spell success
curl -X POST http://192.168.0.193:5001/spell_success

# Reset entire system
curl -X POST http://192.168.0.194:5002/reset
```

## Maintenance and Operation

### Daily Operation
1. Power on all devices in order: Router → Central Pi → Rune Pi → ESP32s
2. Verify system status via Flask endpoints
3. Test critical puzzle paths
4. Monitor logs for errors

### Regular Maintenance  
- Check SD card health and backup configurations
- Update software packages monthly
- Test all maglock functionality
- Verify all sensors and button responsiveness
- Clean connections and check for corrosion

### Backup and Recovery
```bash
# Backup configuration
tar -czf wizards_backup_$(date +%Y%m%d).tar.gz /home/pi/wizards/config/

# Backup entire system (from external computer)
sudo dd if=/dev/mmcblk0 of=pi_backup_$(date +%Y%m%d).img bs=4M status=progress
```

## Security Considerations

- Change default WiFi passwords
- Use secure MQTT authentication if needed
- Implement network segmentation for IoT devices
- Regular security updates on all Pis
- Physical security for control systems

## Customization

### Adding New Puzzles
1. Create new ESP32 code based on existing templates
2. Add MQTT topics to `mqtt_topics.yaml`
3. Update Central Controller logic for new triggers
4. Add maglock/output control as needed
5. Test thoroughly before deployment

### Modifying Game Logic
- Update `system_config.yaml` for timing/sequence changes
- Modify puzzle solution patterns in ESP32 code
- Adjust rune behaviors in Rune Controller
- Update trigger conditions in Central Controller

This system provides a robust, expandable platform for complex wizards experiences with centralized control and distributed sensing.