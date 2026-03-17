#!/usr/bin/env python3
"""
Escape Room Central Controller
==============================

This module serves as the central controller for the escape room system.
It manages all maglocks, coordinates game logic, and serves as the main
MQTT message hub.

Functions:
- Control 9 maglocks via GPIO
- Process MQTT messages from ESP32s and rune controller
- Implement game state management
- Provide HTTP endpoints for game control
- Host MQTT broker coordination

Hardware:
- Raspberry Pi with GPIO pins controlling maglock relays
- MQTT broker (Mosquitto)
- Network connectivity to all ESP32 devices

Author: Escape Room Control System
"""

import time
import threading
import logging
import yaml
import json
from typing import Dict, Any, Optional, List
from datetime import datetime, timedelta
from flask import Flask, request, jsonify
from flask_cors import CORS
import paho.mqtt.client as mqtt
import gpiod

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('/var/log/central_controller.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class CentralController:
    """Main central controller class for escape room management."""
    
    def __init__(self, config_path: str = "/config"):
        """Initialize the central controller."""
        self.config = self._load_config(config_path)
        self.flask_app = Flask(__name__)
        CORS(self.flask_app)
        
        # Game state tracking
        self.game_state = {
            'active': True,
            'start_time': datetime.now(),
            'torch_states': {f'torch_{i}': False for i in range(1, 6)},
            'crystal_states': {
                'red_placed': False,
                'blue_placed': False,  
                'purple_placed': False,
                'white_placed': False,
                'green_placed': False,
                'first_four_complete': False,
                'all_complete': False
            },
            'puzzle_states': {
                'wand_cabinet': False,
                'cross': False,
                'cheese': False,
                'cauldron': False,
                'dials': False,
                'staircase': False,
                'watcher': False
            },
            'maglock_states': {name: False for name in self.config['central_controller']['maglocks'].keys()}
        }
        
        # Initialize hardware
        self._init_gpio()
        
        # Initialize MQTT
        self._init_mqtt()
        
        # Initialize Flask routes
        self._init_flask_routes()
        
        # Start background threads
        self._start_background_threads()
        
        logger.info("Central Controller initialized successfully")
    
    def _load_config(self, config_path: str) -> Dict[str, Any]:
        """Load configuration from YAML files."""
        config = {}
        try:
            with open(f"{config_path}/system_config.yaml", 'r') as f:
                config.update(yaml.safe_load(f))
            with open(f"{config_path}/mqtt_topics.yaml", 'r') as f:
                config.update(yaml.safe_load(f))
            logger.info("Configuration loaded successfully")
        except Exception as e:
            logger.error(f"Failed to load configuration: {e}")
            raise
        return config
    
    def _detect_gpio_chip(self):
        """Detect the correct GPIO chip for this Pi model."""
        import os
        import glob
        
        # First, let's see what GPIO chips are available
        try:
            # List all available GPIO chips
            gpio_devices = glob.glob('/dev/gpiochip*')
            logger.info(f"Available GPIO devices: {gpio_devices}")
            
            # Also check /sys/class/gpio/
            sys_gpio = glob.glob('/sys/class/gpio/gpiochip*')
            logger.info(f"Available sys GPIO chips: {sys_gpio}")
            
        except Exception as e:
            logger.warning(f"Could not list GPIO devices: {e}")
        
        # Try common GPIO chip names
        chip_candidates = ['gpiochip4', 'gpiochip0', 'gpiochip1', 'gpiochip2', 'gpiochip3']
        
        # Also try any chips we found in /dev/
        try:
            dev_chips = [os.path.basename(chip) for chip in glob.glob('/dev/gpiochip*')]
            chip_candidates.extend([chip for chip in dev_chips if chip not in chip_candidates])
            logger.info(f"Trying GPIO chip candidates: {chip_candidates}")
        except Exception as e:
            logger.warning(f"Could not scan /dev/ for GPIO chips: {e}")
        
        for chip_name in chip_candidates:
            try:
                chip = gpiod.Chip(chip_name)
                logger.info(f"Successfully detected GPIO chip: {chip_name}")
                logger.info(f"Chip info - Name: {chip.name}, Label: {chip.label}, Num lines: {chip.num_lines}")
                return chip
            except FileNotFoundError:
                logger.debug(f"GPIO chip {chip_name} not found, trying next...")
                continue
            except Exception as e:
                logger.warning(f"Error trying GPIO chip {chip_name}: {e}")
                continue
        
        # If we still can't find any chips, provide detailed error info
        error_msg = f"No suitable GPIO chip found. Tried: {chip_candidates}"
        try:
            # Add some system info to help debug
            import subprocess
            result = subprocess.run(['ls', '-la', '/dev/gpio*'], capture_output=True, text=True)
            if result.stdout:
                error_msg += f"\n/dev/gpio* contents: {result.stdout}"
        except Exception:
            pass
        
        raise RuntimeError(error_msg)

    def _init_gpio(self):
        """Initialize GPIO pins for maglock control."""
        try:
            # Initialize GPIO chip with automatic detection
            self.gpio_chip = self._detect_gpio_chip()
            
            # Setup maglock pins
            self.maglock_pins = {}
            for maglock_name, pin_num in self.config['central_controller']['maglocks'].items():
                lock_line = self.gpio_chip.get_line(pin_num)
                lock_line.request(consumer="maglock", type=gpiod.LINE_REQ_DIR_OUT)
                lock_line.set_value(0)  # Start with maglocks engaged (LOW)
                
                self.gpio_lines[maglock_name] = lock_line
                self.maglock_pins[maglock_name] = pin_num
            
            # Setup other output pins
            self.other_pins = {}
            for output_name, pin_num in self.config['central_controller']['other_outputs'].items():
                output_line = self.gpio_chip.get_line(pin_num)
                output_line.request(consumer="output", type=gpiod.LINE_REQ_DIR_OUT) 
                output_line.set_value(0)
                
                self.gpio_lines[output_name] = output_line
                self.other_pins[output_name] = pin_num
            
            logger.info("GPIO initialized successfully")
            
        except Exception as e:
            logger.error(f"Failed to initialize GPIO: {e}")
            raise
    
    def _init_mqtt(self):
        """Initialize MQTT client and connection."""
        try:
            self.mqtt_client = mqtt.Client()
            self.mqtt_client.on_connect = self._on_mqtt_connect
            self.mqtt_client.on_message = self._on_mqtt_message
            
            mqtt_config = self.config['mqtt']
            self.mqtt_client.connect(
                mqtt_config['broker_host'], 
                mqtt_config['broker_port'], 
                mqtt_config['keepalive']
            )
            self.mqtt_client.loop_start()
            
            logger.info("MQTT client initialized and connected")
            
        except Exception as e:
            logger.error(f"Failed to initialize MQTT: {e}")
            raise
    
    def _on_mqtt_connect(self, client, userdata, flags, rc):
        """Callback for MQTT connection."""
        if rc == 0:
            logger.info("Connected to MQTT broker")
            # Subscribe to all relevant topics
            topics = [
                # ESP32 topics
                self.config['esp32']['wand_cabinet'],
                self.config['esp32']['cross'],
                self.config['esp32']['cheese'],
                self.config['esp32']['cauldron'],
                self.config['esp32']['dials'],
                self.config['esp32']['staircase'],
                self.config['esp32']['crystals_first_four'],
                self.config['esp32']['crystals_all_placed'],
                self.config['esp32']['watcher'],
                
                # Torch control topics
                self.config['torches']['torch_1'],
                self.config['torches']['torch_2'],
                self.config['torches']['torch_3'],
                self.config['torches']['torch_4'],
                self.config['torches']['torch_5'],
                
                # System topics
                self.config['system']['reset_game'],
                self.config['system']['maintenance'],
                
                # Rune topics that affect maglocks
                self.config['runes']['fire_fireplace'],
                self.config['runes']['dream_rat_cage']
            ]
            
            for topic in topics:
                client.subscribe(topic)
                logger.debug(f"Subscribed to topic: {topic}")
        else:
            logger.error(f"Failed to connect to MQTT broker with result code {rc}")
    
    def _on_mqtt_message(self, client, userdata, msg):
        """Handle incoming MQTT messages."""
        try:
            topic = msg.topic
            payload = msg.payload.decode()
            logger.debug(f"Received MQTT message: {topic} - {payload}")
            
            # Handle ESP32 inputs
            if topic == self.config['esp32']['wand_cabinet']:
                if payload.lower() == 'true':
                    self._unlock_maglock('wand_cabinet')
                    self.game_state['puzzle_states']['wand_cabinet'] = True
            
            elif topic == self.config['esp32']['cross']:
                if payload.lower() == 'true':
                    self._unlock_maglock('purple_crystal_compartment')
                    self.game_state['puzzle_states']['cross'] = True
            
            elif topic == self.config['esp32']['cheese']:
                if payload.lower() == 'true':
                    self._unlock_maglock('rat_trap_door')
                    self.game_state['puzzle_states']['cheese'] = True
            
            elif topic == self.config['esp32']['dials']:
                if payload.lower() == 'true':
                    self._unlock_maglock('treasure_chest')
                    self.game_state['puzzle_states']['dials'] = True
            
            elif topic == self.config['esp32']['staircase']:
                if payload.lower() == 'true':
                    self._unlock_maglock('staircase_compartment')
                    self.game_state['puzzle_states']['staircase'] = True
            
            elif topic == self.config['esp32']['crystals_first_four']:
                if payload.lower() == 'true':
                    self._unlock_maglock('paradox_compartment')
                    self.game_state['crystal_states']['first_four_complete'] = True
            
            elif topic == self.config['esp32']['crystals_all_placed']:
                if payload.lower() == 'true':
                    self._handle_all_crystals_placed()
                    self.game_state['crystal_states']['all_complete'] = True
            
            # Handle torch states for fireplace mantle logic
            elif topic.startswith('escaperoom/torches/torch'):
                torch_num = topic.split('torch')[1]
                self.game_state['torch_states'][f'torch_{torch_num}'] = payload.lower() == 'true'
                self._check_fireplace_mantle_unlock()
            
            # Handle rune-triggered maglocks
            elif topic == self.config['runes']['fire_fireplace']:
                if payload.lower() == 'true':
                    self._unlock_maglock('fireplace_door')
            
            elif topic == self.config['runes']['dream_rat_cage']:
                if payload.lower() == 'true':
                    self._unlock_maglock('rat_cage')
            
            # Handle system commands
            elif topic == self.config['system']['reset_game']:
                if payload.lower() == 'true':
                    self._reset_game()
            
            elif topic == self.config['system']['maintenance']:
                self._handle_maintenance_mode(payload.lower() == 'true')
            
        except Exception as e:
            logger.error(f"Error processing MQTT message: {e}")
    
    def _unlock_maglock(self, maglock_name: str):
        """Unlock a specific maglock."""
        try:
            if maglock_name in self.maglock_pins:
                pin = self.maglock_pins[maglock_name]
                self.gpio_lines[maglock_name].set_value(1)  # HIGH signal unlocks maglock
                self.game_state['maglock_states'][maglock_name] = True
                
                logger.info(f"Unlocked maglock: {maglock_name}")
                
                # Publish status update
                topic = self.config['maglocks'][maglock_name]
                self._publish_mqtt(topic, 'unlocked')
                
        except Exception as e:
            logger.error(f"Failed to unlock maglock {maglock_name}: {e}")
    
    def _lock_maglock(self, maglock_name: str):
        """Lock a specific maglock."""
        try:
            if maglock_name in self.maglock_pins:
                pin = self.maglock_pins[maglock_name]
                self.gpio_lines[maglock_name].set_value(0)  # LOW signal locks maglock
                self.game_state['maglock_states'][maglock_name] = False
                
                logger.info(f"Locked maglock: {maglock_name}")
                
                # Publish status update
                topic = self.config['maglocks'][maglock_name]
                self._publish_mqtt(topic, 'locked')
                
        except Exception as e:
            logger.error(f"Failed to lock maglock {maglock_name}: {e}")
    
    def _check_fireplace_mantle_unlock(self):
        """Check if fireplace mantle should unlock based on torch pattern."""
        solution = self.config['game_logic']['torch_solution']
        current_state = self.game_state['torch_states']
        
        # Check if current state matches solution (1&4 ON, 2&3&5 OFF)
        matches_solution = all(
            current_state.get(f'torch_{i}', False) == solution[f'torch_{i}']
            for i in range(1, 6)
        )
        
        if matches_solution:
            self._unlock_maglock('fireplace_mantle')
            logger.info("Fireplace mantle unlocked - torch puzzle solved correctly!")
    
    def _handle_all_crystals_placed(self):
        """Handle the final crystal placement - trigger win sequence."""
        try:
            logger.info("All crystals placed - initiating win sequence!")
            
            # Trigger final sequence - could unlock another maglock or trigger win condition
            self._publish_mqtt(self.config['game_state']['win_condition'], 'true')
            
            # Update game state
            self.game_state['crystal_states']['all_complete'] = True
            
        except Exception as e:
            logger.error(f"Error handling all crystals placed: {e}")
    
    def _reset_game(self):
        """Reset the entire game system."""
        try:
            logger.info("Resetting game system...")
            
            # Lock all maglocks
            for maglock_name in self.maglock_pins.keys():
                self._lock_maglock(maglock_name)
            
            # Reset game state
            self.game_state = {
                'active': True,
                'start_time': datetime.now(),
                'torch_states': {f'torch_{i}': False for i in range(1, 6)},
                'crystal_states': {
                    'red_placed': False,
                    'blue_placed': False,
                    'purple_placed': False,
                    'white_placed': False,
                    'green_placed': False,
                    'first_four_complete': False,
                    'all_complete': False
                },
                'puzzle_states': {name: False for name in self.game_state['puzzle_states'].keys()},
                'maglock_states': {name: False for name in self.maglock_pins.keys()}
            }
            
            # Publish reset signal to all systems
            self._publish_mqtt(self.config['system']['game_state'], 'RESET')
            
            logger.info("Game system reset completed")
            
        except Exception as e:
            logger.error(f"Error resetting game: {e}")
    
    def _handle_maintenance_mode(self, enable: bool):
        """Handle maintenance mode toggle."""
        if enable:
            logger.info("Entering maintenance mode")
            # Unlock all maglocks for maintenance access
            for maglock_name in self.maglock_pins.keys():
                self._unlock_maglock(maglock_name)
            self.game_state['active'] = False
        else:
            logger.info("Exiting maintenance mode")
            self._reset_game()
    
    def _publish_mqtt(self, topic: str, payload: str):
        """Publish MQTT message."""
        try:
            self.mqtt_client.publish(topic, payload)
            logger.debug(f"Published MQTT: {topic} - {payload}")
        except Exception as e:
            logger.error(f"Failed to publish MQTT message: {e}")
    
    def _init_flask_routes(self):
        """Initialize Flask HTTP routes for game control."""
        
        @self.flask_app.route('/status', methods=['GET'])
        def get_status():
            """Get comprehensive system status."""
            return jsonify({
                'game_state': self.game_state,
                'uptime': (datetime.now() - self.game_state['start_time']).total_seconds(),
                'timestamp': datetime.now().isoformat()
            })
        
        @self.flask_app.route('/maglocks/<maglock_name>/unlock', methods=['POST'])
        def unlock_maglock_endpoint(maglock_name):
            """Manually unlock a specific maglock."""
            if maglock_name in self.maglock_pins:
                self._unlock_maglock(maglock_name)
                return jsonify({'status': 'success', 'maglock': maglock_name, 'action': 'unlocked'})
            else:
                return jsonify({'status': 'error', 'message': 'Invalid maglock name'}), 400
        
        @self.flask_app.route('/maglocks/<maglock_name>/lock', methods=['POST'])
        def lock_maglock_endpoint(maglock_name):
            """Manually lock a specific maglock."""
            if maglock_name in self.maglock_pins:
                self._lock_maglock(maglock_name)
                return jsonify({'status': 'success', 'maglock': maglock_name, 'action': 'locked'})
            else:
                return jsonify({'status': 'error', 'message': 'Invalid maglock name'}), 400
        
        @self.flask_app.route('/maglocks', methods=['GET'])
        def list_maglocks():
            """List all maglocks and their states."""
            maglocks = []
            for name, state in self.game_state['maglock_states'].items():
                maglocks.append({
                    'name': name,
                    'pin': self.maglock_pins[name],
                    'unlocked': state
                })
            return jsonify({'maglocks': maglocks})
        
        @self.flask_app.route('/reset', methods=['POST'])
        def reset_game_endpoint():
            """Reset the entire game system."""
            self._reset_game()
            return jsonify({'status': 'success', 'message': 'Game reset'})
        
        @self.flask_app.route('/maintenance', methods=['POST'])
        def toggle_maintenance():
            """Toggle maintenance mode."""
            data = request.get_json() or {}
            enable = data.get('enable', False)
            self._handle_maintenance_mode(enable)
            return jsonify({
                'status': 'success', 
                'maintenance_mode': enable,
                'message': f"Maintenance mode {'enabled' if enable else 'disabled'}"
            })
        
        @self.flask_app.route('/puzzle/<puzzle_name>/solve', methods=['POST'])
        def solve_puzzle_endpoint(puzzle_name):
            """Manually trigger puzzle solve (for testing)."""
            if puzzle_name in self.game_state['puzzle_states']:
                # Simulate the puzzle being solved
                topic_map = {
                    'wand_cabinet': self.config['esp32']['wand_cabinet'],
                    'cross': self.config['esp32']['cross'],
                    'cheese': self.config['esp32']['cheese'],
                    'dials': self.config['esp32']['dials'],
                    'staircase': self.config['esp32']['staircase']
                }
                
                if puzzle_name in topic_map:
                    self._publish_mqtt(topic_map[puzzle_name], 'true')
                    return jsonify({'status': 'success', 'puzzle': puzzle_name, 'solved': True})
            
            return jsonify({'status': 'error', 'message': 'Invalid puzzle name'}), 400
    
    def _start_background_threads(self):
        """Start background monitoring threads."""
        # System health monitoring
        health_thread = threading.Thread(target=self._health_monitor_loop, daemon=True)
        health_thread.start()
        
        logger.info("Background threads started")
    
    def _health_monitor_loop(self):
        """Monitor system health and log status."""
        while True:
            try:
                # Log system status periodically
                active_maglocks = sum(1 for state in self.game_state['maglock_states'].values() if state)
                solved_puzzles = sum(1 for state in self.game_state['puzzle_states'].values() if state)
                
                logger.info(f"System Health - Active Maglocks: {active_maglocks}/9, Solved Puzzles: {solved_puzzles}")
                
                # Check for win condition
                if self.game_state['crystal_states']['all_complete']:
                    logger.info("WIN CONDITION ACHIEVED!")
                
                time.sleep(60)  # Log every minute
                
            except Exception as e:
                logger.error(f"Error in health monitor: {e}")
                time.sleep(30)
    
    def cleanup(self):
        """Cleanup GPIO and connections."""
        try:
            # Release GPIO lines
            for line in self.gpio_lines.values():
                try:
                    line.release()
                except:
                    pass
            if self.gpio_chip:
                self.gpio_chip.close()
                
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()
            logger.info("Cleanup completed")
        except Exception as e:
            logger.error(f"Error during cleanup: {e}")
    
    def run(self):
        """Run the Flask application."""
        try:
            flask_config = self.config['flask']
            self.flask_app.run(
                host='0.0.0.0',
                port=flask_config['central_controller_port'],
                debug=flask_config['debug']
            )
        except KeyboardInterrupt:
            logger.info("Shutting down central controller...")
        finally:
            self.cleanup()

if __name__ == "__main__":
    # Initialize and run the central controller
    controller = CentralController("/home/pi/Wizards/config")
    controller.run()