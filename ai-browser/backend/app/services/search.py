import os
import httpx
from typing import List, Dict

SEARXNG_URL = os.getenv("SEARXNG_URL", "http://localhost:8888")
GOOGLE_API_KEY = os.getenv("GOOGLE_API_KEY")
GOOGLE_CX = os.getenv("GOOGLE_CX")

async def search_searxng(query: str, limit: int = 10) -> List[Dict]:
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(
                f"{SEARXNG_URL}/search",
                params={"q": query, "format": "json", "engines": "google,bing,duckduckgo"},
            )
            resp.raise_for_status()
        except httpx.HTTPError:
            return []
        results = resp.json()
        return [
            {
                "title": r.get("title", ""),
                "url": r.get("url", ""),
                "snippet": r.get("content", ""),
            }
            for r in results.get("results", [])[:limit]
        ]

async def search_google_cse(query: str, limit: int = 10) -> List[Dict]:
    if not GOOGLE_API_KEY or not GOOGLE_CX:
        return []
    async with httpx.AsyncClient(timeout=10.0) as client:
        try:
            resp = await client.get(
                "https://www.googleapis.com/customsearch/v1",
                params={"key": GOOGLE_API_KEY, "cx": GOOGLE_CX, "q": query, "num": limit},
            )
            resp.raise_for_status()
        except httpx.HTTPError:
            return []
        items = resp.json().get("items", [])
        return [
            {
                "title": i.get("title", ""),
                "url": i.get("link", ""),
                "snippet": i.get("snippet", ""),
            }
            for i in items
        ]

async def search_web(query: str, ai_overview: bool = True) -> Dict:
    results = await search_searxng(query)
    if not results:
        results = await search_google_cse(query)
    return {"query": query, "results": results}
