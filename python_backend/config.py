import os
import json
import sys
from pathlib import Path

# Constants
DEFAULT_CONFIG_PATH = Path("config") / "ai_config.json"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8000

class Config:
    def __init__(self):
        self.provider = "gemini"
        self.model = "gemini-1.5-flash"
        self.api_key = ""
        self.host = DEFAULT_HOST
        self.port = DEFAULT_PORT

    def validate(self):
        if not self.api_key:
            return False, "API Key is missing."
        return True, ""

def load_config() -> Config:
    """
    Loads configuration from 'config/ai_config.json' located relative to the executable.
    Prioritizes file config, allows overrides via Environment Variables.
    """
    cfg = Config()
    
    # Determine base path (works for PyInstaller onefile and normal script)
    if getattr(sys, 'frozen', False):
        base_path = Path(sys.executable).parent
    else:
        base_path = Path(__file__).parent.parent
        
    config_file = base_path / DEFAULT_CONFIG_PATH
    
    # 1. Load from File
    if config_file.exists():
        try:
            with open(config_file, 'r') as f:
                data = json.load(f)
                cfg.provider = data.get("provider", cfg.provider)
                cfg.model = data.get("model", cfg.model)
                cfg.api_key = data.get("api_key", cfg.api_key)
                cfg.host = data.get("host", cfg.host)
                cfg.port = data.get("port", cfg.port)
        except Exception as e:
            print(f"[WARN] Failed to read config file: {e}")

    # 2. Env Var Overrides (Higher Priority)
    if os.environ.get("AI_PROVIDER"): cfg.provider = os.environ["AI_PROVIDER"]
    if os.environ.get("AI_MODEL"): cfg.model = os.environ["AI_MODEL"]
    if os.environ.get("AI_API_KEY"): cfg.api_key = os.environ["AI_API_KEY"]
    if os.environ.get("HOST"): cfg.host = os.environ["HOST"]
    if os.environ.get("PORT"): cfg.port = int(os.environ["PORT"])

    return cfg
