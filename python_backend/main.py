import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from contextlib import asynccontextmanager
import sys
import os

# FORCE PYINSTALLER TO SEE THESE MODULES
try:
    import langchain
    import langchain.chains
    import langchain.memory
    import langchain_community.chat_models
    import langchain_google_genai
    import langchain_openai
except ImportError:
    pass

from config import load_config
from ai_engine import AIEngine

# Global Engine Instance
ai_engine = None
config = None

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    global ai_engine, config
    config = load_config()
    
    # Basic validation
    is_valid, error = config.validate()
    if not is_valid:
        print(f"[ERROR] Config Validation Failed: {error}")
        # We don't exit here to allow the app to run and hopefully report error via API
        # but in production we might want to fail fast.
    
    try:
        ai_engine = AIEngine(config)
        print(f"[INFO] AI Engine Initialized ({config.provider}: {config.model})")
    except Exception as e:
        print(f"[CRITICAL] Failed to initialize AI Engine: {e}")
        ai_engine = None
        
    yield
    # Shutdown logic (if any)
    print("[INFO] Shutting down AI Backend...")

app = FastAPI(lifespan=lifespan)

class ChatRequest(BaseModel):
    text: str

class ChatResponse(BaseModel):
    reply: str

@app.post("/chat", response_model=ChatResponse)
async def chat_endpoint(request: ChatRequest):
    global ai_engine
    
    if not ai_engine:
        raise HTTPException(status_code=503, detail="AI Engine not initialized. Check config/api_key.")
    
    response_text = await ai_engine.get_response(request.text)
    return ChatResponse(reply=response_text)

@app.post("/reset")
async def reset_memory():
    """Clear conversation history"""
    if ai_engine:
        ai_engine.clear_memory()
    return {"status": "memory cleared"}

@app.get("/health")
async def health_check():
    return {"status": "ok", "provider": config.provider if config else "unknown"}

if __name__ == "__main__":
    # Load config just to get port/host for uvicorn
    # Note: lifespan logic runs when uvicorn starts the app
    cfg = load_config()
    
    # Hide banner to run silently
    uvicorn.run(
        app, 
        host=cfg.host, 
        port=cfg.port, 
        log_level="info"
    )
