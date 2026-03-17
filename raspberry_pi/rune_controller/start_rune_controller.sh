#!/bin/bash
"""
Startup script for Rune Controller
=================================

This script sets up the environment and starts the rune controller service.
It should be run at system boot.
"""

# Set working directory
cd /home/pi/escape_room_controller/raspberry_pi/rune_controller

# Activate virtual environment if it exists
if [ -d "/home/pi/escape_room_controller/venv" ]; then
    source /home/pi/escape_room_controller/venv/bin/activate
    echo "Activated Python virtual environment"
fi

# Set environment variables
export PYTHONPATH="/home/pi/escape_room_controller:$PYTHONPATH"
export FLASK_APP="rune_controller.py"
export FLASK_ENV="production"

# Start the rune controller
echo "Starting Rune Controller..."
python3 rune_controller.py