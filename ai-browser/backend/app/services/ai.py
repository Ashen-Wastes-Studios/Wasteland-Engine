import os
import httpx
from typing import List, Dict

LLM_URL = os.getenv("LLM_URL", "http://localhost:11434")
LLM_MODEL = os.getenv("LLM_MODEL", "qwen2.5:3b")

SYSTEM_PROMPT = """You are a concise research assistant. Given search results, produce a 2-4 sentence overview answering the user's query. Cite sources by title in [brackets]. Do not invent URLs."""

async def generate_overview(query: str, results: List[Dict]) -> str:
    context = "\n".join(
        f"- {r['title']}: {r['snippet']}" for r in results[:8] if r.get("snippet")
    )
    prompt = f"Query: {query}\n\nResults:\n{context}\n\nOverview:"
    async with httpx.AsyncClient(timeout=60.0) as client:
        resp = await client.post(
            f"{LLM_URL}/api/chat",
            json={
                "model": LLM_MODEL,
                "stream": False,
                "messages": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": prompt},
                ],
            },
        )
        resp.raise_for_status()
        data = resp.json()
        return data.get("message", {}).get("content", "").strip()
