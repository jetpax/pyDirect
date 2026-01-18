# pyDirect Device Scripts

This directory contains minimal device-side Python scripts for pyDirect firmware.

## Quick Start (Standalone)

For basic pyDirect functionality without Scripto Studio:

1. **Flash firmware** (includes pyDirect modules)
2. **Upload minimal scripts:**
   ```bash
   ./upload-device-scripts.sh /dev/ttyUSB0
   ```
3. **Connect via WebREPL** at `ws://device-ip/webrepl`

## Scripto Studio Integration

For full Scripto Studio IDE integration with advanced features:

```bash
./install-scripto-studio.sh /dev/ttyUSB0
```

This installs:
- Advanced network management (WiFi, Ethernet, WWAN failover)
- Background task system with asyncio
- Settings API
- Client helper functions
- WebRTC signaling
- Status LED management
- And more...

See [Scripto Studio](https://github.com/jetpax/scripto-studio) for details.

## Files

### Minimal (Standalone)
- `boot.py` - Boot configuration
- `main.py` - Minimal server startup

### Full (Scripto Studio)
Installed via `install-scripto-studio.sh`:
- `main.py` - Full orchestrator with async tasks
- `lib/` - Helper modules (network, settings, bg_tasks, etc.)
- `settings/` - Configuration files

## Customization

Edit `main.py` to customize:
- HTTP/HTTPS ports
- WebREPL password
- Certificate paths
- Module initialization
