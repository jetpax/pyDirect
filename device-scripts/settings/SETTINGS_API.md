# Settings API Documentation

The `settings` module provides access to runtime configuration with defaults and user overrides.

## Overview

The settings module manages two layers of configuration:

1. **Default settings** (`/settings/default_settings.json`) - Shipped with firmware, read-only
2. **User settings** (`/settings/user_settings.json`) - Created/modified at runtime

```python
from lib import settings

# Get values (user override, else default)
ssid = settings.get("wifi.ssid")
password = settings.get("wifi.password")

# Set user preferences
settings.set("wifi.ssid", "MyNetwork")
settings.set("wifi.password", "secret123")
settings.save()
```

## Module-Level Functions

### get(key, default=None)

Get a setting value with fallback chain:
1. User override (`/settings/user_settings.json`)
2. Default setting (`/settings/default_settings.json`)
3. Computed default (if applicable)
4. Provided default parameter

```python
ssid = settings.get("wifi.ssid")           # User or default
timeout = settings.get("network.timeout", 5)  # Default to 5 if not found
```

### set(key, value)

Set a user preference. Does not persist until `save()` is called:

```python
settings.set("wifi.ssid", "MyNetwork")
settings.set("wifi.password", "secret123")
settings.set("ethernet.dhcp", True)
```

### save()

Persist user settings to `/settings/user_settings.json`:

```python
settings.set("wifi.ssid", "MyNetwork")
settings.save()  # Writes to disk
```

Returns `True` if successful, `False` on error.

### reload()

Reload settings from disk and clear cache:

```python
settings.reload()
```

Useful after manually editing settings files or receiving updated settings from client.

## Default Settings Structure

Default settings are organized by functional area:

```json
{
  "wifi": {
    "ssid": "",
    "password": "",
    "hostname_prefix": "scripto"
  },
  "ethernet": {
    "dhcp": true,
    "static_ip": null
  },
  "wwan": {
    "apn": "",
    "username": "",
    "password": "",
    "mobile_data_enabled": false
  },
  "server": {
    "webrepl_password": "password",
    "http_port": 80,
    "https_enabled": false,
    "https_cert_file": "/certs/servercert.pem",
    "https_key_file": "/certs/prvtkey.pem"
  },
  "network": {
    "timeout": 5,
    "retries": 3
  },
  "led": {
    "brightness": 0.3,
    "status": "on"
  }
}
```

## User Settings

User settings override defaults. Only modified values need to be stored:

```json
{
  "wifi": {
    "ssid": "MyNetwork",
    "password": "secret123"
  },
  "led": {
    "brightness": 0.5
  }
}
```

## Usage Patterns

### Basic configuration

```python
from lib import settings

# Read settings
ssid = settings.get("wifi.ssid", "")
password = settings.get("wifi.password", "")

if ssid:
    connect_wifi(ssid, password)
```

### Update and persist

```python
# Update settings
settings.set("wifi.ssid", new_ssid)
settings.set("wifi.password", new_password)

# Save to disk
if settings.save():
    print("Settings saved")
else:
    print("Failed to save settings")
```

### Batch updates

```python
# Update multiple settings
updates = {
    "wifi.ssid": "MyNetwork",
    "wifi.password": "secret",
    "network.timeout": 10
}

for key, value in updates.items():
    settings.set(key, value)

settings.save()
```

### Server configuration

```python
# Load server config
webrepl_password = settings.get("server.webrepl_password", "password")
http_port = settings.get("server.http_port", 80)
https_enabled = settings.get("server.https_enabled", False)

# Start servers
webrepl.start(password=webrepl_password)
httpserver.start(port=http_port)
```

## COFU Caching

Settings uses COFU (Construct On First Use) caching:

- First access to a key resolves the value (user → default → computed)
- Result is cached for fast subsequent access
- Cache is cleared on `set()` for modified keys
- Cache is cleared on `reload()`

This means:
- Fast reads after first access
- No redundant file I/O
- Automatic cache invalidation

## Computed Defaults

Some settings can have computed defaults that reference board hardware:

```python
# Example: Sleep interval based on battery capability
interval = settings.get("sleep.interval")
# Returns 60 if board.has("battery"), else 5
```

Computed defaults are evaluated once on first access and cached.

## Error Handling

Settings operations are designed to fail gracefully:

```python
# get() returns None (or default) if key not found
value = settings.get("nonexistent.key")  # None

# save() returns False on error
if not settings.save():
    print("Failed to save settings")

# reload() silently ignores missing files
settings.reload()  # Safe even if files don't exist
```

## File Locations

- `/settings/default_settings.json` - Default settings (shipped with firmware)
- `/settings/user_settings.json` - User overrides (created at runtime)

The `/settings` directory is created automatically when `save()` is first called.

## Integration Examples

### WiFi Helper

```python
def load_wifi_config():
    """Load WiFi configuration from settings."""
    from lib import settings
    
    return {
        'ssid': settings.get("wifi.ssid", ""),
        'password': settings.get("wifi.password", ""),
        'hostname_prefix': settings.get("wifi.hostname_prefix", "scripto")
    }
```

### Ethernet Helper

```python
def load_eth_config():
    """Load Ethernet configuration from settings."""
    from lib import settings
    
    return {
        'dhcp': settings.get("ethernet.dhcp", True),
        'static_ip': settings.get("ethernet.static_ip", None)
    }
```

### Server Startup

```python
def start_servers():
    """Start WebREPL and HTTP server using settings."""
    from lib import settings
    
    # WebREPL
    password = settings.get("server.webrepl_password", "password")
    webrepl.start(password=password)
    
    # HTTP Server
    port = settings.get("server.http_port", 80)
    httpserver.start(port=port)
```

## Best Practices

1. **Use defaults**: Always provide sensible defaults to `get()`
2. **Batch saves**: Group multiple `set()` calls before `save()`
3. **Check board**: Use `board.has()` to conditionally enable features
4. **Validate input**: Check values before calling `set()`
5. **Handle errors**: Check `save()` return value

## See Also

- `lib/board.py` - Hardware definitions (read-only)
- `Boards/README.md` - Board JSON format
- `/settings/default_settings.json` - Default settings file
