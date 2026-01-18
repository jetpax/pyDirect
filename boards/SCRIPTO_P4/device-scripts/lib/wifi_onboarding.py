"""
WiFi Onboarding - Captive Portal Implementation

Provides captive portal for WiFi provisioning when no credentials are stored.
Starts AP mode, runs DNS server, and serves configuration web interface.
"""

import network
import socket
import time
import json

class CaptivePortal:
    """Captive portal for WiFi onboarding"""
    
    # AP configuration
    AP_IP = "192.168.4.1"
    AP_SUBNET = "255.255.255.0"
    AP_GATEWAY = "192.168.4.1"
    DNS_PORT = 53
    
    def __init__(self, ap_ssid, wifi_manager, board_config):
        """
        Initialize captive portal
        
        Args:
            ap_ssid: Access point SSID
            wifi_manager: WiFiManager instance
            board_config: Board configuration module
        """
        self.ap_ssid = ap_ssid
        self.wifi_manager = wifi_manager
        self.board_config = board_config
        self.ap = None
        self.dns_socket = None
        self.running = False
    
    def start(self):
        """Start captive portal (AP mode + DNS server)"""
        print(f"\n{'='*60}")
        print("WiFi Onboarding - Captive Portal")
        print(f"{'='*60}\n")
        
        # Start AP mode
        self.ap = network.WLAN(network.AP_IF)
        self.ap.active(True)
        self.ap.config(essid=self.ap_ssid)
        self.ap.ifconfig((self.AP_IP, self.AP_SUBNET, self.AP_GATEWAY, self.AP_IP))
        
        print(f"✓ Access Point started")
        print(f"  SSID: {self.ap_ssid}")
        print(f"  IP:   {self.AP_IP}")
        print(f"  No password required\n")
        
        # Start DNS server (captive portal redirect)
        self._start_dns_server()
        
        print(f"✓ Captive portal active")
        print(f"\nConnect to '{self.ap_ssid}' and visit:")
        print(f"  http://{self.AP_IP}/setup")
        print(f"  http://setup.local/")
        print(f"\n{'='*60}\n")
        
        self.running = True
    
    def stop(self):
        """Stop captive portal"""
        if self.dns_socket:
            self.dns_socket.close()
            self.dns_socket = None
        
        if self.ap:
            self.ap.active(False)
            self.ap = None
        
        self.running = False
        print("Captive portal stopped")
    
    def _start_dns_server(self):
        """Start DNS server that redirects all queries to AP IP"""
        try:
            self.dns_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.dns_socket.setblocking(False)
            self.dns_socket.bind((self.AP_IP, self.DNS_PORT))
            print(f"✓ DNS server started on {self.AP_IP}:{self.DNS_PORT}")
        except Exception as e:
            print(f"Failed to start DNS server: {e}")
    
    def process_dns(self):
        """Process DNS queries (non-blocking)"""
        if not self.dns_socket:
            return
        
        try:
            data, addr = self.dns_socket.recvfrom(512)
            # Build DNS response redirecting to AP IP
            response = self._build_dns_response(data)
            self.dns_socket.sendto(response, addr)
        except OSError:
            # No data available (non-blocking)
            pass
        except Exception as e:
            print(f"DNS error: {e}")
    
    def _build_dns_response(self, query):
        """Build DNS response redirecting all queries to AP IP"""
        # Simple DNS response: copy query ID, set response flags, add answer
        response = bytearray(query[:2])  # Transaction ID
        response += b'\x81\x80'  # Flags: response, no error
        response += query[4:6]   # Questions count
        response += b'\x00\x01'  # Answers count
        response += b'\x00\x00\x00\x00'  # Authority + Additional
        response += query[12:]   # Original question
        
        # Answer: point to question name, type A, class IN, TTL 60s
        response += b'\xc0\x0c'  # Pointer to question name
        response += b'\x00\x01'  # Type A
        response += b'\x00\x01'  # Class IN
        response += b'\x00\x00\x00\x3c'  # TTL 60 seconds
        response += b'\x00\x04'  # Data length
        
        # IP address (AP_IP)
        ip_parts = self.AP_IP.split('.')
        response += bytes([int(p) for p in ip_parts])
        
        return bytes(response)
    
    def get_setup_page_html(self):
        """Generate setup page HTML"""
        hostname = self.board_config.get_hostname()
        
        html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi Setup - {hostname}</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }}
        .container {{
            background: white;
            border-radius: 16px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 480px;
            width: 100%;
            padding: 40px;
        }}
        h1 {{
            color: #333;
            margin-bottom: 10px;
            font-size: 28px;
        }}
        .subtitle {{
            color: #666;
            margin-bottom: 30px;
            font-size: 14px;
        }}
        .form-group {{
            margin-bottom: 20px;
        }}
        label {{
            display: block;
            margin-bottom: 8px;
            color: #555;
            font-weight: 500;
            font-size: 14px;
        }}
        select, input {{
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            font-size: 16px;
            transition: border-color 0.3s;
        }}
        select:focus, input:focus {{
            outline: none;
            border-color: #667eea;
        }}
        button {{
            width: 100%;
            padding: 14px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }}
        button:hover {{
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }}
        button:active {{
            transform: translateY(0);
        }}
        .scan-btn {{
            background: #f0f0f0;
            color: #333;
            margin-bottom: 20px;
        }}
        .scan-btn:hover {{
            background: #e0e0e0;
            box-shadow: none;
        }}
        .loading {{
            display: none;
            text-align: center;
            padding: 20px;
            color: #666;
        }}
        .spinner {{
            border: 3px solid #f3f3f3;
            border-top: 3px solid #667eea;
            border-radius: 50%;
            width: 40px;
            height: 40px;
            animation: spin 1s linear infinite;
            margin: 0 auto 10px;
        }}
        @keyframes spin {{
            0% {{ transform: rotate(0deg); }}
            100% {{ transform: rotate(360deg); }}
        }}
        .network-item {{
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            margin-bottom: 8px;
            cursor: pointer;
            transition: all 0.2s;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }}
        .network-item:hover {{
            border-color: #667eea;
            background: #f8f9ff;
        }}
        .network-item.selected {{
            border-color: #667eea;
            background: #f0f3ff;
        }}
        .signal-strength {{
            color: #888;
            font-size: 12px;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>WiFi Setup</h1>
        <p class="subtitle">Configure WiFi for {hostname}</p>
        
        <button class="scan-btn" onclick="scanNetworks()">🔍 Scan for Networks</button>
        
        <div id="loading" class="loading">
            <div class="spinner"></div>
            <p>Scanning for networks...</p>
        </div>
        
        <form id="wifiForm" onsubmit="submitForm(event)">
            <div class="form-group">
                <label for="ssid">WiFi Network</label>
                <select id="ssid" name="ssid" required>
                    <option value="">-- Select Network --</option>
                </select>
            </div>
            
            <div class="form-group">
                <label for="password">Password</label>
                <input type="password" id="password" name="password" placeholder="Enter WiFi password">
            </div>
            
            <button type="submit">Connect to WiFi</button>
        </form>
    </div>
    
    <script>
        async function scanNetworks() {{
            document.getElementById('loading').style.display = 'block';
            document.getElementById('wifiForm').style.display = 'none';
            
            try {{
                const response = await fetch('/api/scan');
                const networks = await response.json();
                
                const select = document.getElementById('ssid');
                select.innerHTML = '<option value="">-- Select Network --</option>';
                
                networks.forEach(net => {{
                    const option = document.createElement('option');
                    option.value = net.ssid;
                    option.textContent = `${{net.ssid}} (${{net.rssi}} dBm)`;
                    select.appendChild(option);
                }});
                
                document.getElementById('loading').style.display = 'none';
                document.getElementById('wifiForm').style.display = 'block';
            }} catch (e) {{
                alert('Scan failed: ' + e);
                document.getElementById('loading').style.display = 'none';
                document.getElementById('wifiForm').style.display = 'block';
            }}
        }}
        
        async function submitForm(e) {{
            e.preventDefault();
            
            const ssid = document.getElementById('ssid').value;
            const password = document.getElementById('password').value;
            
            if (!ssid) {{
                alert('Please select a network');
                return;
            }}
            
            try {{
                const response = await fetch('/api/configure', {{
                    method: 'POST',
                    headers: {{'Content-Type': 'application/json'}},
                    body: JSON.stringify({{ssid, password}})
                }});
                
                const result = await response.json();
                
                if (result.success) {{
                    window.location.href = '/success?ssid=' + encodeURIComponent(ssid);
                }} else {{
                    alert('Configuration failed: ' + result.error);
                }}
            }} catch (e) {{
                alert('Error: ' + e);
            }}
        }}
    </script>
</body>
</html>"""
        return html
    
    def get_success_page_html(self, ssid):
        """Generate success page HTML with mDNS URL and QR code"""
        hostname = self.board_config.get_hostname()
        mdns_url = f"http://{hostname}.local/"
        
        # Simple QR code placeholder (actual QR generation would use qrcode library)
        qr_data = f"QR:{mdns_url}"
        
        html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi Configured!</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }}
        .container {{
            background: white;
            border-radius: 16px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 480px;
            width: 100%;
            padding: 40px;
            text-align: center;
        }}
        .success-icon {{
            font-size: 64px;
            margin-bottom: 20px;
        }}
        h1 {{
            color: #333;
            margin-bottom: 10px;
            font-size: 28px;
        }}
        .subtitle {{
            color: #666;
            margin-bottom: 30px;
            font-size: 14px;
        }}
        .steps {{
            text-align: left;
            background: #f8f9fa;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 20px;
        }}
        .step {{
            margin-bottom: 15px;
            padding-left: 30px;
            position: relative;
        }}
        .step::before {{
            content: attr(data-num);
            position: absolute;
            left: 0;
            top: 0;
            width: 24px;
            height: 24px;
            background: #11998e;
            color: white;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 12px;
            font-weight: bold;
        }}
        .url {{
            background: #e8f5e9;
            padding: 15px;
            border-radius: 8px;
            margin: 20px 0;
            word-break: break-all;
        }}
        .url a {{
            color: #11998e;
            text-decoration: none;
            font-weight: 600;
            font-size: 18px;
        }}
        .qr-placeholder {{
            background: #f0f0f0;
            padding: 20px;
            border-radius: 8px;
            margin: 20px 0;
            color: #666;
            font-size: 14px;
        }}
        button {{
            padding: 12px 24px;
            background: #11998e;
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            margin: 5px;
        }}
    </style>
</head>
<body>
    <div class="container">
        <div class="success-icon">✓</div>
        <h1>WiFi Configured!</h1>
        <p class="subtitle">Successfully connected to {ssid}</p>
        
        <div class="steps">
            <div class="step" data-num="1">
                <strong>Reconnect to your WiFi network</strong><br>
                <small>Switch back to "{ssid}" on your device</small>
            </div>
            <div class="step" data-num="2">
                <strong>Visit the device URL</strong><br>
                <small>Use the link below or scan the QR code</small>
            </div>
        </div>
        
        <div class="url">
            <a href="{mdns_url}" target="_blank">{mdns_url}</a>
        </div>
        
        <div class="qr-placeholder">
            📱 QR Code<br>
            <small>Scan with your phone to open {hostname}.local</small><br>
            <small style="font-family: monospace; margin-top: 10px; display: block;">{qr_data}</small>
        </div>
        
        <button onclick="window.location.href='{mdns_url}'">Open Device Page</button>
        <button onclick="copyUrl()">Copy URL</button>
    </div>
    
    <script>
        function copyUrl() {{
            navigator.clipboard.writeText('{mdns_url}');
            alert('URL copied to clipboard!');
        }}
    </script>
</body>
</html>"""
        return html
