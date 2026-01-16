
import os

_BRANDING_CUSTOM_PATH = None
_BRANDING_NAME = None

def set_custom_module_dir(custom_module_dir: str):
    """Set the path to the custom branding module."""
    global _BRANDING_CUSTOM_PATH
    _BRANDING_CUSTOM_PATH = custom_module_dir

def get_custom_module_dir() -> str:
    """Get the path to the custom branding module."""
    if _BRANDING_CUSTOM_PATH is None:
        raise ValueError("Custom module directory is not set.")
    return _BRANDING_CUSTOM_PATH

def set_branding_name(branding_name: str):
    """Set the branding name used for replacements."""
    global _BRANDING_NAME
    _BRANDING_NAME = branding_name

def get_branding_name() -> str:
    """Get the branding name used for replacements."""
    if _BRANDING_NAME is None:
        raise ValueError("Branding name is not set.")
    return _BRANDING_NAME

def get_branding_config_path() -> str:
    """Get the path to the branding configuration file."""
    custom_module_dir = get_custom_module_dir()
    return os.path.join(custom_module_dir, "config", "branding.json")
