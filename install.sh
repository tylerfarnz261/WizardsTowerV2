#!/bin/bash
"""
Escape Room Controller - Installation Script
============================================

This script automates the installation process for the Raspberry Pi controllers.
Run this on each Raspberry Pi to set up the escape room control system.

Usage: ./install.sh [rune|central]

Author: Escape Room Control System
"""

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
PROJECT_DIR="/home/pi/Wizards"
VENV_DIR="$PROJECT_DIR/venv"
USER="pi"

# Print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as pi user
check_user() {
    if [[ $USER != "pi" ]]; then
        print_warning "This script should be run as the 'pi' user"
        read -p "Continue anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
}

# Update system packages
update_system() {
    print_status "Updating system packages..."
    sudo apt update
    sudo apt upgrade -y
    print_success "System packages updated"
}

# Install required system packages
install_system_packages() {
    print_status "Installing system packages..."
    sudo apt install -y \
        python3-pip \
        python3-venv \
        python3-dev \
        git \
        i2c-tools \
        mosquitto \
        mosquitto-clients \
        curl \
        htop \
        vim
    print_success "System packages installed"
}

# Create Python virtual environment
setup_python_env() {
    print_status "Setting up Python virtual environment..."
    
    if [[ ! -d "$VENV_DIR" ]]; then
        python3 -m venv "$VENV_DIR"
    fi
    
    source "$VENV_DIR/bin/activate"
    pip install --upgrade pip
    pip install -r "$PROJECT_DIR/requirements.txt"
    
    print_success "Python environment configured"
}

# Enable I2C (for rune controller)
enable_i2c() {
    print_status "Enabling I2C interface..."
    
    # Check if I2C is already enabled
    if grep -q "^dtparam=i2c_arm=on" /boot/config.txt; then
        print_success "I2C already enabled"
    else
        echo "dtparam=i2c_arm=on" | sudo tee -a /boot/config.txt
        echo "i2c-dev" | sudo tee -a /etc/modules
        print_warning "I2C enabled - reboot required"
    fi
}

# Setup MQTT broker (for central controller)
setup_mqtt_broker() {
    print_status "Setting up MQTT broker..."
    
    # Copy configuration
    sudo cp "$PROJECT_DIR/config/mosquitto.conf" /etc/mosquitto/conf.d/escape_room.conf
    
    # Enable and start Mosquitto
    sudo systemctl enable mosquitto
    sudo systemctl restart mosquitto
    
    # Test broker
    timeout 5s mosquitto_sub -h localhost -t test/topic &
    sleep 1
    mosquitto_pub -h localhost -t test/topic -m "Installation test"
    sleep 1
    
    print_success "MQTT broker configured"
}

# Install systemd service
install_service() {
    local controller_type=$1
    
    print_status "Installing $controller_type service..."
    
    case $controller_type in
        "rune")
            # Make script executable
            chmod +x "$PROJECT_DIR/raspberry_pi/rune_controller/start_rune_controller.sh"
            
            # Copy service file
            sudo cp "$PROJECT_DIR/raspberry_pi/rune_controller/rune-controller.service" /etc/systemd/system/
            
            # Enable service
            sudo systemctl daemon-reload
            sudo systemctl enable rune-controller
            
            print_success "Rune controller service installed"
            ;;
            
        "central")
            # Make script executable
            chmod +x "$PROJECT_DIR/raspberry_pi/central_controller/start_central_controller.sh"
            
            # Copy service file  
            sudo cp "$PROJECT_DIR/raspberry_pi/central_controller/central-controller.service" /etc/systemd/system/
            
            # Enable service
            sudo systemctl daemon-reload
            sudo systemctl enable central-controller
            
            print_success "Central controller service installed"
            ;;
            
        *)
            print_error "Unknown controller type: $controller_type"
            exit 1
            ;;
    esac
}

# Configure network settings
configure_network() {
    print_status "Configuring network settings..."
    
    # Set hostname based on controller type
    local controller_type=$1
    local new_hostname="escaperoom-$controller_type"
    
    if [[ $(hostname) != "$new_hostname" ]]; then
        print_status "Setting hostname to $new_hostname"
        echo "$new_hostname" | sudo tee /etc/hostname
        sudo sed -i "s/127.0.1.1.*/127.0.1.1\t$new_hostname/g" /etc/hosts
        print_warning "Hostname changed - reboot required"
    fi
}

# Create log directories
setup_logging() {
    print_status "Setting up logging..."
    
    sudo mkdir -p /var/log/escaperoom
    sudo chown pi:pi /var/log/escaperoom
    
    # Setup log rotation
    cat << EOF | sudo tee /etc/logrotate.d/escaperoom
/var/log/escaperoom/*.log {
    daily
    missingok
    rotate 7
    compress
    delaycompress
    copytruncate
}
EOF
    
    print_success "Logging configured"
}

# Test installation
test_installation() {
    local controller_type=$1
    
    print_status "Testing installation..."
    
    case $controller_type in
        "rune")
            # Test I2C
            if command -v i2cdetect >/dev/null 2>&1; then
                print_success "I2C tools available"
            else
                print_error "I2C tools not found"
            fi
            
            # Test Python imports
            source "$VENV_DIR/bin/activate"
            python3 -c "import flask, paho.mqtt.client, yaml; print('Python dependencies OK')" 2>/dev/null && \
                print_success "Python dependencies available" || \
                print_error "Python dependency issues"
                
            # Test service
            if sudo systemctl is-enabled rune-controller >/dev/null 2>&1; then
                print_success "Rune controller service enabled"
            else
                print_error "Rune controller service not enabled"
            fi
            ;;
            
        "central")
            # Test MQTT broker
            if mosquitto_pub -h localhost -t test/topic -m "test" 2>/dev/null; then
                print_success "MQTT broker responding"
            else
                print_error "MQTT broker not responding"
            fi
            
            # Test Python imports  
            source "$VENV_DIR/bin/activate"
            python3 -c "import flask, paho.mqtt.client, RPi.GPIO; print('Python dependencies OK')" 2>/dev/null && \
                print_success "Python dependencies available" || \
                print_error "Python dependency issues"
                
            # Test service
            if sudo systemctl is-enabled central-controller >/dev/null 2>&1; then
                print_success "Central controller service enabled"
            else
                print_error "Central controller service not enabled"  
            fi
            ;;
    esac
}

# Show final instructions
show_final_instructions() {
    local controller_type=$1
    
    echo
    print_success "Installation completed for $controller_type controller!"
    echo
    echo "Next steps:"
    echo "1. Update IP addresses in config/system_config.yaml"
    echo "2. Configure hardware connections"
    
    if [[ $controller_type == "rune" ]]; then
        echo "3. Connect MCP23017 GPIO expanders via I2C"
        echo "4. Test I2C connection: sudo i2cdetect -y 1"
    elif [[ $controller_type == "central" ]]; then
        echo "3. Connect relay modules to GPIO pins"
        echo "4. Test MQTT broker: mosquitto_sub -t escaperoom/+/+"
    fi
    
    echo "5. Start service: sudo systemctl start ${controller_type}-controller"
    echo "6. Check logs: journalctl -u ${controller_type}-controller -f"
    echo "7. Test HTTP endpoints (see documentation)"
    echo
    
    if grep -q "reboot required" /tmp/install.log 2>/dev/null; then
        print_warning "Reboot required for all changes to take effect"
        read -p "Reboot now? (y/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            sudo reboot
        fi
    fi
}

# Main installation function
main() {
    local controller_type=$1
    
    # Validate argument
    if [[ -z "$controller_type" ]] || [[ ! "$controller_type" =~ ^(rune|central)$ ]]; then
        echo "Usage: $0 [rune|central]"
        echo "  rune    - Install rune controller"
        echo "  central - Install central controller" 
        exit 1
    fi
    
    print_status "Starting installation for $controller_type controller..."
    
    # Create log file
    exec 1> >(tee -a /tmp/install.log)
    exec 2>&1
    
    check_user
    update_system
    install_system_packages
    setup_python_env
    
    if [[ $controller_type == "rune" ]]; then
        enable_i2c
    elif [[ $controller_type == "central" ]]; then
        setup_mqtt_broker
    fi
    
    install_service "$controller_type" 
    configure_network "$controller_type"
    setup_logging
    test_installation "$controller_type"
    show_final_instructions "$controller_type"
}

# Run main function with all arguments
main "$@"