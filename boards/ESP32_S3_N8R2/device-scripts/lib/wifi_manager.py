"""
WiFi Manager - Credential Storage and Connection Management

Handles WiFi credential storage in NVS (Non-Volatile Storage) and connection management.
"""

import network
import time

try:
    import esp
    NVS_AVAILABLE = True
except ImportError:
    NVS_AVAILABLE = False
    print("WARNING: NVS not available, using in-memory storage")

class WiFiManager:
    """Manage WiFi credentials and connections"""
    
    # NVS namespace for WiFi credentials
    NVS_NAMESPACE = "wifi"
    NVS_SSID_KEY = "ssid"
    NVS_PASSWORD_KEY = "password"
    
    def __init__(self):
        self.wlan = network.WLAN(network.STA_IF)
        self._cached_ssid = None
        self._cached_password = None
    
    def has_credentials(self):
        """Check if WiFi credentials are stored"""
        if NVS_AVAILABLE:
            try:
                nvs = esp.NVS(self.NVS_NAMESPACE)
                ssid = nvs.get_str(self.NVS_SSID_KEY)
                return ssid is not None and len(ssid) > 0
            except:
                return False
        else:
            return self._cached_ssid is not None
    
    def save_credentials(self, ssid, password):
        """Save WiFi credentials to NVS"""
        if NVS_AVAILABLE:
            try:
                nvs = esp.NVS(self.NVS_NAMESPACE)
                nvs.set_str(self.NVS_SSID_KEY, ssid)
                nvs.set_str(self.NVS_PASSWORD_KEY, password)
                nvs.commit()
                print(f"WiFi credentials saved: {ssid}")
                return True
            except Exception as e:
                print(f"Failed to save credentials: {e}")
                return False
        else:
            self._cached_ssid = ssid
            self._cached_password = password
            print(f"WiFi credentials cached (NVS not available): {ssid}")
            return True
    
    def load_credentials(self):
        """Load WiFi credentials from NVS"""
        if NVS_AVAILABLE:
            try:
                nvs = esp.NVS(self.NVS_NAMESPACE)
                ssid = nvs.get_str(self.NVS_SSID_KEY)
                password = nvs.get_str(self.NVS_PASSWORD_KEY)
                return (ssid, password) if ssid else (None, None)
            except:
                return (None, None)
        else:
            return (self._cached_ssid, self._cached_password)
    
    def clear_credentials(self):
        """Clear stored WiFi credentials"""
        if NVS_AVAILABLE:
            try:
                nvs = esp.NVS(self.NVS_NAMESPACE)
                nvs.erase_key(self.NVS_SSID_KEY)
                nvs.erase_key(self.NVS_PASSWORD_KEY)
                nvs.commit()
                print("WiFi credentials cleared")
                return True
            except:
                return False
        else:
            self._cached_ssid = None
            self._cached_password = None
            print("WiFi credentials cleared (cache)")
            return True
    
    def connect(self, ssid=None, password=None, timeout=30):
        """
        Connect to WiFi network
        
        Args:
            ssid: WiFi SSID (if None, loads from NVS)
            password: WiFi password (if None, loads from NVS)
            timeout: Connection timeout in seconds
            
        Returns:
            IP address if connected, None if failed
        """
        # Load credentials if not provided
        if ssid is None or password is None:
            ssid, password = self.load_credentials()
            if not ssid:
                print("No WiFi credentials available")
                return None
        
        # Activate WiFi
        self.wlan.active(True)
        
        # Check if already connected to this network
        if self.wlan.isconnected():
            current_ssid = self.wlan.config('essid')
            if current_ssid == ssid:
                ip = self.wlan.ifconfig()[0]
                print(f"Already connected to {ssid}: {ip}")
                return ip
            else:
                print(f"Disconnecting from {current_ssid}")
                self.wlan.disconnect()
                time.sleep(1)
        
        # Connect to network
        print(f"Connecting to WiFi: {ssid}...")
        self.wlan.connect(ssid, password)
        
        # Wait for connection
        start_time = time.time()
        while not self.wlan.isconnected():
            if time.time() - start_time > timeout:
                print(f"Connection timeout after {timeout}s")
                return None
            time.sleep(0.5)
        
        ip = self.wlan.ifconfig()[0]
        print(f"WiFi connected: {ip}")
        return ip
    
    def disconnect(self):
        """Disconnect from WiFi"""
        if self.wlan.isconnected():
            self.wlan.disconnect()
            print("WiFi disconnected")
        self.wlan.active(False)
    
    def scan(self):
        """
        Scan for available WiFi networks
        
        Returns:
            List of tuples: (ssid, bssid, channel, rssi, security, hidden)
        """
        self.wlan.active(True)
        networks = self.wlan.scan()
        
        # Sort by signal strength (RSSI)
        networks.sort(key=lambda x: x[3], reverse=True)
        
        return networks
    
    def get_status(self):
        """Get current WiFi status"""
        return {
            'active': self.wlan.active(),
            'connected': self.wlan.isconnected(),
            'ssid': self.wlan.config('essid') if self.wlan.isconnected() else None,
            'ip': self.wlan.ifconfig()[0] if self.wlan.isconnected() else None,
            'rssi': self.wlan.status('rssi') if self.wlan.isconnected() else None
        }
