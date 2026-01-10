import os
from langchain.chains import ConversationChain
from langchain.memory import ConversationBufferMemory
from langchain_google_genai import ChatGoogleGenerativeAI
from langchain_openai import ChatOpenAI
from langchain_core.messages import SystemMessage
from config import Config

class AIEngine:
    def __init__(self, config: Config):
        self.config = config
        self.memory = ConversationBufferMemory()
        self.llm = self._initialize_llm()
        self.chain = self._initialize_chain()

    def _initialize_llm(self):
        """Initializes the LLM based on configuration provider."""
        if self.config.provider == "gemini":
            if not self.config.api_key:
                raise ValueError("Gemini API Key is missing")
            
            # Configure GenAI
            os.environ["GOOGLE_API_KEY"] = self.config.api_key
            return ChatGoogleGenerativeAI(
                model=self.config.model,
                temperature=0.7,
                convert_system_message_to_human=True # Known fix for some Gemini versions
            )
            
        elif self.config.provider == "openai":
            if not self.config.api_key:
                raise ValueError("OpenAI API Key is missing")
            
            os.environ["OPENAI_API_KEY"] = self.config.api_key
            return ChatOpenAI(
                model=self.config.model,
                temperature=0.7
            )
        else:
            raise ValueError(f"Unsupported provider: {self.config.provider}")

    def _initialize_chain(self):
        """Creates a ConversationChain with persistent memory."""
        # System prompt to define personality
        system_prompt = (
            "You are a helpful and concise AI assistant running inside a desktop overlay. "
            "Keep your answers short, direct, and relevant to the user's context. "
            "Do not be verbose unless asked."
        )
        
        # Note: We can add a system message to memory or use a prompt template.
        # For simplicity with ConversationChain:
        return ConversationChain(
            llm=self.llm,
            memory=self.memory,
            verbose=False
        )

    async def get_response(self, user_text: str) -> str:
        """Video Processes input and returns AI response."""
        try:
            response = await self.chain.apredict(input=user_text)
            return response
        except Exception as e:
            return f"Error generating response: {str(e)}"

    def clear_memory(self):
        """Resets the conversation history."""
        self.memory.clear()
