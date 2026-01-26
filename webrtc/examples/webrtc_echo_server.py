"""
WebRTC Echo Server Example

This example demonstrates browser-to-ESP32 communication using WebRTC DataChannels.
The ESP32 acts as the WebRTC peer and echoes back any data received from the browser.

Features:
- HTTPS signaling via httpserver module
- DataChannel for bidirectional communication
- Automatic certificate trust workflow
- Echo server pattern

Hardware Required:
- ESP32-S3 or ESP32-P4 with WiFi

Prerequisites:
1. Build firmware with MODULE_PYDIRECT_WEBRTC=ON
2. Generate device certificate (see ../../docs/HTTPS_WSS_COMPLETE.md)
3. Upload certificate to /certs/ on device
4. Configure WiFi credentials below

Usage:
1. Upload this file to device
2. Run: import webrtc_echo_server
3. From browser, visit: https://device.local/trust-cert (accept certificate)
4. Open browser console and run the test client code (see bottom of file)
5. Send messages from browser, see echoes!
"""

import webrtc
import httpserver
import json
import time

# Configuration
WIFI_SSID = 'YourWiFiSSID'
WIFI_PASSWORD = 'YourWiFiPassword'

# Global state
peer = None
signaling_data = {
    'local_offer': None,
    'local_candidates': [],
    'ready': False
}

def setup_wifi():
    """Connect to WiFi"""
    import network
    sta = network.WLAN(network.STA_IF)
    if not sta.isconnected():
        print('Connecting to WiFi...')
        sta.active(True)
        sta.connect(WIFI_SSID, WIFI_PASSWORD)
        
        timeout = 10
        while not sta.isconnected() and timeout > 0:
            time.sleep(1)
            timeout -= 1
        
        if not sta.isconnected():
            print('ERROR: WiFi connection failed!')
            return False
    
    print('WiFi connected!')
    print('IP:', sta.ifconfig()[0])
    return True

def setup_webrtc():
    """Initialize WebRTC peer"""
    global peer, signaling_data
    
    print('Initializing WebRTC peer...')
    
    # Create peer in controlling role (generates offer)
    peer = webrtc.Peer(role='controlling')
    
    # Callback: Local SDP offer generated
    def on_offer(sdp):
        print(f'Local offer generated ({len(sdp)} bytes)')
        signaling_data['local_offer'] = sdp
        signaling_data['ready'] = True
    
    # Callback: Local ICE candidate generated
    def on_ice(candidate):
        print(f'ICE candidate: {candidate[:60]}...')
        signaling_data['local_candidates'].append(candidate)
    
    # Callback: Data received from browser
    def on_data(data):
        try:
            msg = data.decode('utf-8')
            print(f'Received: {msg}')
            
            # Echo back with prefix
            response = f'ESP32 echo: {msg}'
            peer.send(response.encode('utf-8'))
            print(f'Sent: {response}')
        except Exception as e:
            print(f'Error handling data: {e}')
    
    # Callback: Connection state changed
    def on_state(state):
        state_names = {
            webrtc.STATE_NONE: 'NONE',
            webrtc.STATE_CONNECTING: 'CONNECTING',
            webrtc.STATE_CONNECTED: 'CONNECTED',
            webrtc.STATE_DATA_CHANNEL_CONNECTED: 'DATA_CHANNEL_CONNECTED',
            webrtc.STATE_DATA_CHANNEL_OPENED: 'DATA_CHANNEL_OPENED',
            webrtc.STATE_DISCONNECTED: 'DISCONNECTED'
        }
        state_name = state_names.get(state, f'UNKNOWN({state})')
        print(f'State: {state_name}')
        
        if state == webrtc.STATE_DATA_CHANNEL_OPENED:
            print('✅ DataChannel ready! Browser can now send messages.')
            # Send welcome message
            peer.send(b'Hello from ESP32! Send me a message.')
    
    # Register callbacks
    peer.on_offer(on_offer)
    peer.on_ice(on_ice)
    peer.on_data(on_data)
    peer.on_state(on_state)
    
    # Start connection (generate local offer)
    print('Creating offer...')
    peer.create_offer()
    
    # Wait for offer to be generated
    print('Waiting for offer generation...')
    timeout = 5
    while not signaling_data['ready'] and timeout > 0:
        time.sleep(0.1)
        timeout -= 0.1
    
    if not signaling_data['ready']:
        print('ERROR: Offer generation timed out')
        return False
    
    print('WebRTC peer ready!')
    return True

def setup_signaling_endpoint():
    """Set up HTTPS endpoint for WebRTC signaling"""
    
    def handle_webrtc_signal(req, resp):
        """
        Handle WebRTC signaling messages from browser
        
        Protocol:
        - Browser sends: {type: 'get_offer'} -> we return our offer
        - Browser sends: {type: 'answer', sdp: '...'} -> we process answer
        - Browser sends: {type: 'ice', candidate: '...'} -> we add ICE candidate
        """
        global peer, signaling_data
        
        try:
            body = req.read()
            msg = json.loads(body)
            
            if msg.get('type') == 'get_offer':
                # Browser requests our offer
                if not signaling_data['ready']:
                    resp.send_json({
                        'error': 'Peer not ready yet',
                        'status': 'not_ready'
                    })
                    return
                
                print('Sending offer to browser...')
                resp.send_json({
                    'type': 'offer',
                    'sdp': signaling_data['local_offer'],
                    'candidates': signaling_data['local_candidates']
                })
            
            elif msg.get('type') == 'answer':
                # Browser sends answer
                print('Received answer from browser')
                peer.set_remote_sdp(msg['sdp'])
                
                # Process any ICE candidates from browser
                if 'candidates' in msg:
                    for candidate in msg['candidates']:
                        peer.add_ice_candidate(candidate)
                
                resp.send_json({'status': 'ok'})
            
            elif msg.get('type') == 'ice':
                # Browser sends ICE candidate
                print('Received ICE candidate from browser')
                peer.add_ice_candidate(msg['candidate'])
                resp.send_json({'status': 'ok'})
            
            else:
                resp.send_json({'error': 'Unknown message type'})
        
        except Exception as e:
            print(f'Signaling error: {e}')
            resp.send_json({'error': str(e)})
    
    def handle_trust_cert(req, resp):
        """Simple page for trusting certificate"""
        html = '''
<!DOCTYPE html>
<html>
<head>
    <title>WebRTC Echo Server</title>
    <style>
        body {
            font-family: -apple-system, system-ui, sans-serif;
            padding: 40px;
            max-width: 800px;
            margin: 0 auto;
        }
        .success { color: #27ae60; font-size: 48px; }
        button {
            background: #3498db;
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 6px;
            font-size: 16px;
            cursor: pointer;
            margin-top: 20px;
        }
        button:hover { background: #2980b9; }
        #output {
            background: #ecf0f1;
            padding: 15px;
            border-radius: 6px;
            margin-top: 20px;
            font-family: monospace;
            white-space: pre-wrap;
            max-height: 300px;
            overflow-y: auto;
        }
        .hidden { display: none; }
    </style>
</head>
<body>
    <h1><span class="success">✓</span> Certificate Trusted!</h1>
    <p>Your browser now trusts this device's certificate.</p>
    <p>You can now establish WebRTC connections.</p>
    
    <h2>Test WebRTC Echo</h2>
    <button onclick="testWebRTC()">Connect via WebRTC</button>
    <button onclick="sendMessage()" id="sendBtn" class="hidden">Send Test Message</button>
    <div id="output"></div>
    
    <script>
        let dataChannel = null;
        const output = document.getElementById('output');
        const sendBtn = document.getElementById('sendBtn');
        
        function log(msg) {
            output.textContent += msg + '\\n';
            output.scrollTop = output.scrollHeight;
        }
        
        async function testWebRTC() {
            try {
                log('Creating RTCPeerConnection...');
                const pc = new RTCPeerConnection({
                    iceServers: [] // LAN only
                });
                
                // Create data channel
                dataChannel = pc.createDataChannel('echo', {
                    ordered: true,
                    maxRetransmits: 3
                });
                
                dataChannel.onopen = () => {
                    log('✅ DataChannel open!');
                    sendBtn.classList.remove('hidden');
                };
                
                dataChannel.onmessage = (event) => {
                    log('📨 ' + event.data);
                };
                
                // Gather ICE candidates
                const candidates = [];
                pc.onicecandidate = (event) => {
                    if (event.candidate) {
                        candidates.push(event.candidate.candidate);
                    }
                };
                
                // Get offer from ESP32
                log('Requesting offer from ESP32...');
                const offerResp = await fetch('/webrtc-signal', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({type: 'get_offer'})
                });
                const offerData = await offerResp.json();
                
                if (offerData.error) {
                    log('ERROR: ' + offerData.error);
                    return;
                }
                
                log('Setting remote offer...');
                await pc.setRemoteDescription({
                    type: 'offer',
                    sdp: offerData.sdp
                });
                
                // Add remote ICE candidates
                for (const candidate of offerData.candidates) {
                    await pc.addIceCandidate({candidate: candidate});
                }
                
                // Create answer
                log('Creating answer...');
                const answer = await pc.createAnswer();
                await pc.setLocalDescription(answer);
                
                // Wait for ICE gathering
                await new Promise(resolve => {
                    if (pc.iceGatheringState === 'complete') resolve();
                    else pc.onicegatheringstatechange = () => {
                        if (pc.iceGatheringState === 'complete') resolve();
                    };
                });
                
                // Send answer back
                log('Sending answer to ESP32...');
                await fetch('/webrtc-signal', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({
                        type: 'answer',
                        sdp: answer.sdp,
                        candidates: candidates
                    })
                });
                
                log('Waiting for DataChannel to open...');
            } catch (err) {
                log('ERROR: ' + err.message);
                console.error(err);
            }
        }
        
        function sendMessage() {
            if (dataChannel && dataChannel.readyState === 'open') {
                const msg = 'Hello at ' + new Date().toLocaleTimeString();
                log('➡️ ' + msg);
                dataChannel.send(msg);
            } else {
                log('ERROR: DataChannel not open');
            }
        }
    </script>
</body>
</html>
        '''
        resp.send_html(html)
    
    httpserver.register('/webrtc-signal', handle_webrtc_signal)
    httpserver.register('/trust-cert', handle_trust_cert)
    httpserver.register('/', handle_trust_cert)  # Default page
    
    print('Signaling endpoint registered at /webrtc-signal')
    print('Test page available at /trust-cert')

def main():
    """Main entry point"""
    print('='*60)
    print('WebRTC Echo Server Example')
    print('='*60)
    
    # Step 1: Connect to WiFi
    if not setup_wifi():
        return
    
    # Step 2: Initialize WebRTC
    if not setup_webrtc():
        return
    
    # Step 3: Set up signaling endpoint
    setup_signaling_endpoint()
    
    print('')
    print('='*60)
    print('✅ WebRTC Echo Server Running!')
    print('='*60)
    print('')
    print('Next steps:')
    print('1. Find your device IP/hostname from the output above')
    print('2. Visit https://device.local/ in your browser')
    print('   (or use the IP address shown above)')
    print('3. Accept the self-signed certificate warning')
    print('4. Click "Connect via WebRTC"')
    print('5. Click "Send Test Message" to test echo')
    print('')
    print('The ESP32 will echo back any messages you send!')
    print('='*60)

# Auto-run on import
if __name__ == '__main__':
    main()
else:
    # Run when imported
    main()
