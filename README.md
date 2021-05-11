# hw3
## About the project:
This project is about using mbed to run a RPC loop with two custom functions (operation modes): (1) gesture UI, and (2) tilt angle detection
## Built with:
C++, Python
## Equipment
1. PC
2. B_L4S5I_IOT01A
3. uLCD display
## Description
This project is embedding in B_L4S5I_IOT01A, the PC/Python use RPC over serial to send a command to call gesture UI mode on mbed.Thus, the gesture UI function
will start a thread function.
In the thread function, user will use gesture to select from a few threshold angles.fter the selection is confirmed with a user button, 
the selected threshold angle is published through WiFi/MQTT.
After the PC/Python get the published confirmation from the broker, it sends a command to mbed to stop the guest UI mode.
For the other mode, PC/Python use RPC over serial to send a command to call tilt angle detection mode on mbed. In this mode, we can initialize the reference vector, than tilt the
device. If the angle over the selected threshold angle, mbed will publish the event and angle through WiFi/MQTT to a broker and then send a commend to stop.
