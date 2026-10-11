#!/bin/bash
nmcli con mod eth0-static ipv4.addresses "192.168.1.100/24, 192.168.0.100/24" ipv4.gateway ""
nmcli con up eth0-static
ip addr show dev eth0
